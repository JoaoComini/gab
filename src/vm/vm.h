#ifndef GAB_VM_H
#define GAB_VM_H

#include "arena.h"
#include "diagnostics.h"
#include "scope.h"
#include "string/string_pool.h"
#include "util/list.h"
#include "value.h"
#include "vm/chunk.h"

#include <stdint.h>

/*
    Encodes R-type instructions in a 32-bit integer
    op: OpCode (7-bit)
    rd: Destination register (8-bit)
    r1: Register 1 (8-bit)
    r2: Register 2 (8-bit)
    k:  when set, r2 is a small unsigned immediate rather than a register

    The k bit is Lua's register-or-constant operand trick: 'x + 1' would
    otherwise need a LOAD_CONST into a register the arithmetic then reads once
    and never again, and small literals are most of what arithmetic operates on.
    Only the second operand can be immediate, which is enough because the
    commutative ops are emitted with the constant on the right.
*/
#define VM_ENCODE_R(op, rd, r1, r2) VM_ENCODE_RK(op, rd, r1, r2, 0)

/*
    As VM_ENCODE_R, plus the spare k bit. Every field is masked: an
    out-of-range value would otherwise smear into its neighbours — including
    the opcode — and produce an instruction that matches no case.
*/
#define VM_ENCODE_RK(op, rd, r1, r2, k)                                                                      \
    ((((op) & 0x7F) << 25) | (((rd) & 0xFF) << 17) | (((r1) & 0xFF) << 9) | (((r2) & 0xFF) << 1) |           \
     ((k) & 0x1))

#define VM_DECODE_R_RD(instr) (((instr) >> 17) & 0xFF) // Destination register
#define VM_DECODE_R_R1(instr) (((instr) >> 9) & 0xFF)  // First source register
#define VM_DECODE_R_R2(instr) (((instr) >> 1) & 0xFF)  // Second source register
#define VM_DECODE_R_K(instr) ((instr) & 0x1)           // r2 is an immediate, not a register

// The widest immediate the r2 field holds. A literal above this is loaded into
// a register as before, so the range is a codegen decision and never a limit on
// what a program can say.
#define VM_MAX_IMMEDIATE 0xFF

/*
    Encodes I-type instructions in a 32-bit integer
    op: OpCode (7-bit)
    rd: Destination register (8-bit)
    kx | imm: Constant index (17 bit) or immediate value
*/
#define VM_ENCODE_I(op, rd, kx) ((((op) & 0x7F) << 25) | (((rd) & 0xFF) << 17) | ((kx) & 0x1FFFF))

#define VM_DECODE_I_RD(instr) (((instr) >> 17) & 0xFF) // Destination register
#define VM_DECODE_I_KX(instr) ((instr) & 0x1FFFF)      // 17-bit constant/index
#define VM_DECODE_I_IMM(instr) ((instr) & 0x1FFFF)     // 17-bit immediate value

#define VM_DECODE_OPCODE(instr) ((instr) >> 25) // Get OpCode (default to all types)

// Maximum constants supported by 17-bit index
#define VM_MAX_CONSTANTS ((1 << 17) - 1)

// Maximum registers supported with 8-bit
#define VM_MAX_REGISTERS ((1 << 8) - 1)

// OP_RETURN_N carries its slot count in the 8-bit r2 field.
#define VM_MAX_RETURN_SLOTS ((1 << 8) - 1)

// A pointer is a raw address, so it spans two slots and wants an even slot
// index to sit at its natural alignment.
#define VM_POINTER_SLOTS ((unsigned int)(sizeof(void *) / sizeof(Value)))

// Sentinel value for registers
#define VM_INVALID_REGISTER VM_MAX_REGISTERS + 1

typedef struct {
    Chunk *chunk;
    int arity;
    int max_registers;
} FuncPrototype;

#define VM_MAX_CALL_DEPTH 256

typedef struct {
    const FuncPrototype *proto;

    size_t return_ip;

    // Byte offset into the stack, not a slot index.
    size_t base;
    unsigned int dest;
} CallFrame;

// What a compile produces: the top-level chunk plus the frame size it needs.
// The two travel together because running the chunk means building a prototype
// from it, and only codegen knows how many registers that takes.
typedef struct {
    Chunk *chunk;
    unsigned int max_registers;

    // The module the unit declared, interned in the VM's pool, or NULL for the
    // root namespace. Carried out because the AST that held it is destroyed
    // inside vm_compile.
    String *module_name;
} CompiledScript;

void func_proto_free(FuncPrototype proto);

#define func_proto_list_item_free(item) func_proto_free(item)
GAB_LIST(FuncProtoList, func_proto_list, FuncPrototype)


void vm_compiled_script_free(CompiledScript *script);

// A unit the VM has loaded, kept so its top-level chunk can be freed with the
// VM and replaced when the same name is loaded again. The name is the one the
// host passed to gab_load, copied because the host's string need not outlive
// the call.
typedef struct {
    char name[128];
    CompiledScript script;
} LoadedScript;

#define loaded_script_list_item_free(item) vm_compiled_script_free(&(item).script)
GAB_LIST(LoadedScriptList, loaded_script_list, LoadedScript)

// Every GabFunc this VM has handed out. A handle points into the VM's arena, so
// it cannot outlive the VM in any case; owning them here makes that the actual
// rule rather than a lifetime the host has to manage, and is why there is no
// way to release one early.
//
// Held as void * because GabFunc is the embedding API's type and opaque here.
// gab.c does the freeing, which is why the item_free hook is empty.
#define func_handle_list_item_free(item) ((void)(item))
GAB_LIST(FuncHandleList, func_handle_list, void *)

// Module name to the scope holding its declarations. Keyed on the interned
// name, so identity comparison is enough.
#define module_scope_map_hash(key) (size_t)key
#define module_scope_map_key_equals(key, other) key == other
#define module_scope_map_key_dup(key) key
#define module_scope_map_entry_free(key, value)

GAB_HASH_MAP(ModuleScopeMap, module_scope_map, String *, Scope *)

// Why a run stopped. A run that completed normally leaves VM_RUN_OK; anything
// else means the interpreter unwound early, and the frames are already gone.
typedef enum {
    VM_RUN_OK,
    VM_RUN_ERR_CALL_DEPTH,
    VM_RUN_ERR_STACK_OVERFLOW,
} VmRunStatus;

// The interpreter's failure channel. A run cannot report through a return value
// because it unwinds from inside the loop, so the reason is left here for
// whoever started the run to read. 'message' is a static string, not owned.
typedef struct {
    VmRunStatus status;
    const char *message;
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
typedef struct {
    // Lives until vm_free: interned strings, global symbols, and every Type.
    Arena *arena;

    // Reset at the start of every compile: the AST, diagnostics, and the scopes
    // of blocks, none of which anything holds once codegen is done. Kept on the
    // VM rather than created per compile so its blocks are recycled instead of
    // returned to malloc between compiles.
    Arena *compile_arena;

    StringPool strings; // must outlive global_scope

    // The root namespace: builtins, plus the declarations of any unit that
    // named no module. Every module scope parents to it, so builtins resolve
    // from anywhere and the type registry is shared.
    Scope global_scope;

    // One scope per declared module name, created on first use and living as
    // long as the VM. A module accumulates across compiles: a second unit
    // naming the same module compiles against the scope the first one filled.
    ModuleScopeMap *module_scopes;

    // Bumped once per compile, and stamped onto every symbol that compile
    // declares. It is what tells a redeclaration apart from a reload: the same
    // generation means one unit declared the name twice, which is an error,
    // while an older one means a previous compile did and this one replaces it.
    unsigned int compile_generation;

    // Function prototypes are VM-wide because a prototype index is baked into
    // OP_CALL operands. Top-level variables are not here: they are frame-zero
    // locals on the stack, so top-level state lives and dies with a run.
    FuncProtoList global_funcs;

    // The handles this VM has handed out. See FuncHandleList.
    FuncHandleList func_handles;

    // Every unit loaded into this VM, by name. See LoadedScript.
    LoadedScriptList scripts;

    // The stack is byte-addressed and 8-byte aligned at the base, so a value
    // wider than a slot can sit at its natural alignment. Capacity is still
    // counted in slots; a slot is sizeof(Value).
    uint8_t *stack;
    size_t stack_capacity;

    // Points at stack + frame->base, in bytes.
    uint8_t *registers;

    CallFrame frames[VM_MAX_CALL_DEPTH];
    size_t frame_count;

    size_t instruction_pointer;

    // Why the last run stopped. Cleared at the start of every run.
    VmError error;
} VM;

// Register r of the current frame. A union member read gives well-defined type
// punning, which a cast from the raw byte pointer would not.
static inline Value *vm_reg(const VM *vm, size_t r) {
    return (Value *)(void *)(vm->registers + r * sizeof(Value));
}

// Slot i counted from the base of the stack, independent of any frame.
static inline Value *vm_slot(const VM *vm, size_t i) {
    return (Value *)(void *)(vm->stack + i * sizeof(Value));
}

VM *vm_create();
void vm_free(VM *vm);

// Compiles without running, so a script can be compiled once and run many
// times. Returns false and leaves 'out' untouched if any stage failed; the
// diagnostics say why. On success the caller owns out->chunk and must pass it
// to vm_compiled_script_free.
//
// Diagnostics are allocated from the VM's compile arena, which the next compile
// reclaims — so they stay readable until then, but not past it.
bool vm_compile(VM *vm, const char *source, CompiledScript *out, Diagnostics *diagnostics);

// The scope holding a module's declarations, created on first mention and
// living as long as the VM. NULL names the root namespace.
Scope *vm_module_scope(VM *vm, String *name);

// Lookup-only, for the resolver: NULL when no such module exists.
Scope *vm_module_scope_lookup(void *ctx, String *name);

// Runs a compiled script as frame zero, leaving its result in slot 0. Returns
// why the run stopped; vm->error carries the same status plus a message.
// Nothing is printed — reporting belongs to the caller.
VmRunStatus vm_run(VM *vm, const CompiledScript *script);

// Pushes one frame and runs the interpreter until it unwinds. The result is
// left at the frame's own r0, which is the slot at base. The embedding API
// calls in through this; base is a byte offset into the stack, and the caller
// must already have placed the arguments in the parameter slots above it.
VmRunStatus vm_run_frame(VM *vm, const FuncPrototype *proto, size_t base, unsigned int dest);


// Compile, run, and discard, reporting any diagnostics to stderr. The
// convenience path for a caller with nothing to say about failure.
void vm_execute(VM *vm, const char *source);

#endif
