#ifndef GAB_LINK_H
#define GAB_LINK_H

#include "arena.h"
#include "diagnostics.h"
#include "string/string.h"
#include "type.h"
#include "util/list.h"
#include "vm/chunk.h"

#include <stddef.h>

struct Symbol;

// unwind jumps past every free codegen emitted, so the frames have to be told
// what to drop.
//
// A 'ref T' slot is never listed: it borrows, so there is nothing to free.
typedef struct {
    unsigned int slot;
} FrameRef;

#define frame_ref_list_item_free(item) ((void)(item))
GAB_LIST(FrameRefList, frame_ref_list, FrameRef)

// An 'extern' function's host body. Takes the frame it was called with, so it
// reads its arguments and writes its result through the slots already in
// place — the same layout a script function's frame has, which is what keeps
// the boundary free of marshalling.
typedef struct GabArgs GabArgs;
typedef void (*GabExternFn)(GabArgs *args);

// What the VM calls the same struct. The tag is the host's because gab.h
// declares it opaque under that name and must stay self-contained; the
// accessors a host reaches it through are the checked tier over the VM's own.
typedef struct GabArgs Args;

// A body written in C rather than in bytecode, which is both a builtin type's
// method and a host extern. Answers false when the run must unwind.
//
// One signature for both, so OP_CALL has a single native path: a builtin sits
// in this slot itself, and a host body sits behind a wrapper that adapts it.
typedef bool (*NativeFn)(Args *args);

// One callable function. A unit's top level is one of these too, with an arity
// of zero and no host body: it runs as frame zero, so the interpreter has a
// single path and OP_RETURN means the same thing everywhere.
typedef struct {
    // NULL for a function whose body is C, which runs through 'native'
    // instead. Exactly one of the two is set, and OP_CALL branches on which.
    Chunk *chunk;

    NativeFn native;

    // The host body, for an extern. Held apart from 'native' because a host
    // writes to the GabExternFn signature, which knows nothing of a VM: what
    // sits in 'native' is the wrapper that calls this one.
    GabExternFn extern_body;

    // The declaration a C body was written against, for resolving a parameter
    // index to its slot. Set for an extern and for a builtin method; a script
    // function's frame is addressed by the code codegen emitted, which needs no
    // signature at run time. Points into the environment's arena, so it
    // outlives every compile.
    const struct Symbol *extern_symbol;

    int arity;
    int max_registers;

    // Every slot this function ever owns a reference in. Walked only on an
    // abnormal unwind, where the ordinary releases are skipped — so it costs
    // nothing on the path that matters, and the alternative is leaking whatever
    // the stack held when the run failed.
    //
    // A slot may appear here and hold something else by the time a failure
    // happens, since sibling blocks reuse slots. That is why the runtime clears
    // a slot when it releases it: a slot listed here either holds a live
    // reference or holds NULL, and NULL is what both release paths tolerate.
    FrameRefList refs;
} FuncPrototype;

void func_proto_free(FuncPrototype *proto);

// Prototypes are held by pointer, not by value: a frame addresses its prototype
// for as long as it runs, and this list grows whenever a unit loads. Each
// prototype comes from the environment's arena, so the list frees what a prototype owns
// and not the prototype itself.
#define func_proto_list_item_free(item) func_proto_free(item)
GAB_LIST(FuncProtoList, func_proto_list, FuncPrototype *)

// The types OP_NEW can allocate. Types are owned by the scope arena and
// outlive every compile, so the list holds borrowed pointers and frees none.
#define type_list_item_free(item) ((void)(item))
GAB_LIST(TypeList, type_list, const Type *)

// The literals OP_LOAD_STR can load. Interned in the VM's pool, so equal text
// is one String * and the list holds borrowed pointers.
#define string_list_item_free(item) ((void)(item))
GAB_LIST(StringList, string_list, String *)

// One operand a unit must rewrite once linking tells it where its indices
// landed. A unit numbers what it declares from zero, so an operand it encoded
// means nothing until the unit's base is added to it.
//
// A reference to something an earlier unit declared is already absolute and is
// never recorded here: only what this unit numbered itself gets rebased.
typedef struct {
    Chunk *chunk;
    size_t offset;
} Relocation;

#define relocation_list_item_free(item) ((void)(item))
GAB_LIST(RelocationList, relocation_list, Relocation)

// A prototype this unit declared, and the symbol to stamp with its index once
// linking makes that index absolute. Stamping at link rather than during
// codegen is what keeps a compile that fails from leaving a symbol pointing at
// a prototype nothing installed.
typedef struct {
    struct Symbol *symbol;
    size_t local_index;
} ProtoBinding;

#define proto_binding_list_item_free(item) ((void)(item))
GAB_LIST(ProtoBindingList, proto_binding_list, ProtoBinding)

// An 'extern' declaration awaiting a host body. Resolved at link, so a unit
// that names a body nothing supplied installs nothing rather than leaving a
// prototype behind that a call could reach unbound.
typedef struct {
    size_t local_index;
    const struct Symbol *symbol;
    Span span;
} ExternRequest;

#define extern_request_list_item_free(item) ((void)(item))
GAB_LIST(ExternRequestList, extern_request_list, ExternRequest)

// What a compile produces, before any of it belongs to a VM.
//
// Everything here is numbered from zero and owned by the unit, so a compile
// that fails is discarded by freeing this and nothing else. Linking appends the
// unit's prototypes and types to the program's, rebases every operand the unit
// recorded, resolves its externs, and only then stamps the symbols -- so the program
// either gains the whole unit or is untouched by it.
typedef struct {
    // The unit's top level, which the caller keeps once the rest has linked.
    // Everything below is consumed by the link and gone with the unit.
    FuncPrototype top_level;

    FuncProtoList prototypes;
    TypeList types;
    StringList strings;

    RelocationList proto_relocations;
    RelocationList type_relocations;
    RelocationList string_relocations;

    ProtoBindingList bindings;
    ExternRequestList externs;

    // The arena the unit allocates from, held so that anything the link needs
    // can be allocated after codegen has finished.
    Arena *arena;

    // Where each of the unit's types landed in the program's list, filled in by the
    // link check so that installing has nothing left that can fail.
    size_t *type_map;
    size_t *string_map;
} Unit;

void unit_free(Unit *unit);

// The top level of every unit the program has loaded, kept only so its chunk
// is freed with the program. Nothing looks one up: a unit is reached through
// the names it declared, never through the file it came from.
#define top_level_list_item_free(item) func_proto_free(&(item))
GAB_LIST(TopLevelList, top_level_list, FuncPrototype)

// A host body bound to a name, waiting for a script to declare it 'extern'.
// Registrations outlive every load, so one made before any unit loads is still
// there for the last of them.
typedef struct {
    String *module;
    String *name;
    GabExternFn fn;
} ExternBinding;

#define extern_binding_list_item_free(item) ((void)(item))
GAB_LIST(ExternBindingList, extern_binding_list, ExternBinding)

// What has been loaded and can be run: the image a link installs into and an
// instruction indexes. Everything here is named by index from bytecode, which
// is why it is one table per kind rather than per module -- an operand carries
// no module, so the numbering has to be program-wide.
//
// Allocated out of Environment::arena, so a Program never outlives the
// Environment that compiled it.
typedef struct {
    // Function prototypes are program-wide because a prototype index is baked
    // into OP_CALL operands. Top-level variables are not here: they are
    // frame-zero locals on the stack, so top-level state lives and dies with a
    // run.
    FuncProtoList prototypes;

    // Where the prototypes a unit loaded begin. The VM registers the builtin
    // types' methods below this, once, before any unit loads -- so a loaded
    // function's index is its position in the list, not its position among the
    // functions its own module declared.
    size_t builtin_proto_count;

    // The types OP_NEW allocates, indexed from the instruction. A Type * is 8
    // bytes and the constant pool holds 4-byte Values, so an allocation names
    // its type by index the same way a call names its prototype. Interning is
    // by pointer identity, which the type system already guarantees.
    TypeList heap_types;

    // The literals OP_LOAD_STR loads, indexed from the instruction. A String *
    // is 8 bytes, so a literal names itself by index the way an allocation
    // names its type.
    StringList strings;

    // Every loaded unit's top level. See TopLevelList.
    TopLevelList top_levels;

    // Host bodies bound by name, resolved against a unit's 'extern'
    // declarations as it loads. See ExternBinding.
    ExternBindingList externs;
} Program;

// Whether this unit could be installed. Reports through the diagnostics sink if
// an index would not fit its operand field once rebased, or an "extern" names a
// body nothing registered. Touches nothing on the program, so a caller may ask and
// then discard the unit.
bool link_check(Program *program, Unit *unit, Diagnostics *diagnostics);

// Installs a unit that link_check has accepted. Cannot fail.
void link_install(Program *program, Unit *unit);

#endif
