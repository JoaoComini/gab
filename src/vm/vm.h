#ifndef GAB_VM_H
#define GAB_VM_H

#include "arena.h"
#include "diagnostics.h"
#include "scope.h"
#include "slot.h"
#include "string/string_pool.h"
#include "util/list.h"
#include "vm/chunk.h"
#include "vm/link.h"
#include "vm/opcode.h"

#include <stdint.h>
#include <string.h>

#define VM_MAX_CALL_DEPTH 256

typedef struct {
    const FuncPrototype *proto;

    ptrdiff_t return_ip;

    // Byte offset into the stack, not a slot index.
    size_t base;
    unsigned int dest;
} CallFrame;

// Every GabFunc this VM has handed out. A handle points into the VM's arena, so
// it cannot outlive the VM in any case; owning them here makes that the actual
// rule rather than a lifetime the host has to manage, and is why there is no
// way to release one early.
//
// Held as void * because GabFunc is the embedding API's type and opaque here.
// gab.c does the freeing, which is why the item_free hook is empty.
#define func_handle_list_item_free(item) ((void)(item))
GAB_LIST(FuncHandleList, func_handle_list, void *)

// One module naming another, recorded as each unit links. The graph a host
// would otherwise have to keep for itself: what a unit depends on is what it
// imported, and this is where that becomes something the VM can be asked.
//
// Both names are interned, so identity is the comparison.
typedef struct {
    String *from;
    String *to;
} ModuleImport;

#define module_import_list_item_free(item) ((void)(item))
GAB_LIST(ModuleImportList, module_import_list, ModuleImport)

// StringList comes from link.h, which the program's literal table also uses.

// Why a run stopped. A run that completed normally leaves VM_RUN_OK; anything
// else means the interpreter unwound early, and the frames are already gone.
typedef enum {
    VM_RUN_OK,
    VM_RUN_ERR_CALL_DEPTH,
    VM_RUN_ERR_STACK_OVERFLOW,
    VM_RUN_ERR_OUT_OF_MEMORY,
    VM_RUN_ERR_DIVIDE_BY_ZERO,
    VM_RUN_ERR_DIVIDE_OVERFLOW,

    // An 'extern' function reported failure, or none was ever bound to the
    // prototype a call named.
    VM_RUN_ERR_EXTERN,
} VmRunStatus;

// The interpreter's failure channel. A run cannot report through a return value
// because it unwinds from inside the loop, so the reason is left here for
// whoever started the run to read.
//
// The message is copied rather than pointed at. Most are literals, but an
// extern's comes from the host and may be a local of the call that reported it,
// so one rule for all of them beats a pointer whose lifetime depends on which
// status it carries. Sized to match GabError::message, which is where a host
// eventually reads it.
typedef struct {
    VmRunStatus status;
    char message[256];
} VmError;

// Arenas are named for what owns them, and the rule that follows from that is
// the whole lifetime model: allocate from the arena of the thing that will own
// the result. A Type published into the registry is owned by the VM; an AST
// node is owned by the compile that built it.
//
// A third, module-scoped lifetime belongs between these two — long enough to
// outlive a compile, short enough to be freed by gab_module_free. It does not
// exist yet because every compile shares one global scope and type registry,
// so a module's types are reachable from the next module and freeing them
// would dangle. It becomes real alongside per-module scopes.
//
// The compile-time world a unit resolves against: every name that exists, the
// scopes they live in, and which module may see which. A run never reads any of
// it -- what a run needs, codegen has already turned into an index.
typedef struct {
    // Lives until vm_free: interned strings, global symbols, and every Type.
    // Also backs the prototypes in Program, which is why neither outlives this.
    Arena *arena;

    // Reset at the start of every compile: the AST, diagnostics, and the scopes
    // of blocks, none of which anything holds once codegen is done. Kept here
    // rather than created per compile so its blocks are recycled instead of
    // returned to malloc between compiles.
    Arena *compile_arena;

    StringPool strings; // must outlive global_scope

    // The builtins, and nothing else: no unit declares into this scope. Every
    // module scope parents to it, so 'int' and its siblings resolve from
    // anywhere and the type registry is shared.
    Scope global_scope;

    // One scope per declared module name, created on first use and living as
    // long as the VM. A module accumulates across compiles: a second unit
    // naming the same module compiles against the scope the first one filled.
    ModuleScopeMap *module_scopes;

    // Which module imports which. See ModuleImport.
    ModuleImportList module_imports;
} Environment;

// The machine: a stack, a frame array, and where in the bytecode it is. Holds
// the Environment and Program by value for now, because one arena backs all
// three and vm_free is the single lifetime -- but the interpreter reads only
// 'program', so what it takes to run is already separable from what it took to
// compile.
typedef struct VM {
    Environment env;
    Program program;

    // The handles this VM has handed out. See FuncHandleList.
    FuncHandleList func_handles;

    // The stack is byte-addressed and 8-byte aligned at the base, so a value
    // wider than a slot can sit at its natural alignment. Capacity is still
    // counted in slots; a slot is VM_SLOT_SIZE bytes.
    uint8_t *stack;
    size_t stack_capacity;

    // Points at stack + frame->base, in bytes.
    uint8_t *registers;

    CallFrame frames[VM_MAX_CALL_DEPTH];
    size_t frame_count;

    // Signed, because a jump offset is: an index that went negative wraps to a
    // huge unsigned value, which reads as 'past the end' and would end the run
    // quietly instead of tripping a bound.
    ptrdiff_t instruction_pointer;

    // Why the last run stopped. Cleared at the start of every run.
    VmError error;
} VM;

// One C body's view of the frame it was called with, shared by a builtin
// method and a host extern. Opaque to a host, which reaches its arguments
// through the gab_arg_get_* accessors; the VM's own bodies read the same slots
// through the args_* ones.
struct GabArgs {
    VM *vm;

    // The function being called, which is what the native wrapper reads to find
    // the host body it stands in front of.
    const FuncPrototype *proto;

    // The declaration this call was made against, which is what turns a
    // parameter index into a slot and says how wide the return value is.
    const struct Symbol *symbol;

    // Byte offset of the frame's slot 0, which is where the return value goes;
    // the arguments follow it, laid out exactly as a script callee's would be.
    size_t base;

    // Set when the extern reported failure, which unwinds the run once it
    // returns. A C function cannot longjmp out of the interpreter safely, so
    // the failure is recorded and acted on at the boundary.
    bool failed;
};

// Where register r of the current frame begins, and where slot i of the stack
// begins. Bytes, because a slot is a size rather than a type: what lives there
// is whatever the static types said, and only the accessors below name a width.
static inline uint8_t *vm_reg_at(const VM *vm, size_t r) { return vm->registers + r * VM_SLOT_SIZE; }

static inline uint8_t *vm_slot_at(const VM *vm, size_t i) { return vm->stack + i * VM_SLOT_SIZE; }

// Scalar reads and writes. Every one goes through memcpy, which is the only
// way to move bytes into a typed object without assuming the alignment or the
// effective type of what they came from -- and which every compiler folds into
// the single load or store it describes.
//
// The pointer pair is the same operation over two slots, and sits here rather
// than apart so that every access to a slot reads alike.
static inline int32_t vm_read_i32(const VM *vm, size_t r) {
    int32_t value;
    memcpy(&value, vm_reg_at(vm, r), sizeof(value));

    return value;
}

static inline float vm_read_f32(const VM *vm, size_t r) {
    float value;
    memcpy(&value, vm_reg_at(vm, r), sizeof(value));

    return value;
}

static inline void vm_write_i32(VM *vm, size_t r, int32_t value) {
    memcpy(vm_reg_at(vm, r), &value, sizeof(value));
}

static inline void vm_write_f32(VM *vm, size_t r, float value) {
    memcpy(vm_reg_at(vm, r), &value, sizeof(value));
}

static inline uint8_t *vm_read_ptr(const VM *vm, size_t r) {
    uint8_t *address;
    memcpy(&address, vm_reg_at(vm, r), sizeof(address));

    return address;
}

static inline void vm_write_ptr(VM *vm, size_t r, uint8_t *address) {
    memcpy(vm_reg_at(vm, r), &address, sizeof(address));
}

VM *vm_create();
void vm_free(VM *vm);

// The scope holding a module's declarations, created on first mention and
// living as long as the VM. NULL names the default module.
Scope *environment_module_scope(Environment *env, String *name);

#endif
