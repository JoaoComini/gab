#ifndef GAB_H
#define GAB_H

// The only header a host includes. Everything here is opaque on purpose: a
// host embedding Gab should never need to know what a Type, a Symbol, or a
// Value is, and nothing in this file drags those definitions in.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GabVM GabVM;
typedef struct GabModule GabModule;
typedef struct GabFunc GabFunc;
typedef struct GabType GabType;

typedef enum {
    GAB_OK,
    GAB_ERR_COMPILE,
    GAB_ERR_RUNTIME,
    GAB_ERR_ARG,
} GabStatus;

// The message is copied rather than pointed at, so a GabError outlives the
// compile that produced it. Only the first error is reported this way; a host
// that wants every message can still walk the diagnostics.
typedef struct {
    char message[256];
    int line;
    int column;
} GabError;

// --- Lifecycle -------------------------------------------------------------

GabVM *gab_vm_new(void);
void gab_vm_free(GabVM *vm);

// --- Compiling and running -------------------------------------------------

// Compiles without running, so a module compiles once and runs every frame.
// Returns NULL on failure and fills 'err' if it is non-NULL. Nothing is
// printed: reporting is the host's business.
GabModule *gab_compile(GabVM *vm, const char *name, const char *src, GabError *err);

// Runs the module's top level. Safe to call repeatedly.
GabStatus gab_module_run(GabVM *vm, GabModule *mod, GabError *err);

// Frees the compilation unit and its top-level chunk. The function prototypes
// it added stay for the VM's life: a prototype index is baked into OP_CALL
// operands, so they cannot be reclaimed per unit. Unloading is therefore not
// yet supported.
void gab_module_free(GabVM *vm, GabModule *mod);

// The module the unit declared, or NULL if it declared none and so belongs to
// the root namespace. A host reads the namespace back rather than assuming it
// matches the 'name' it passed to gab_compile — the script is what names the
// module, and the two need not agree.
const char *gab_module_name(const GabModule *mod);

// --- Types and layout ------------------------------------------------------

// A script struct's layout is the C layout, which is the whole zero-
// marshalling story: these let a host check that against its own sizeof and
// offsetof rather than trusting it.
//
// 'mod' selects the namespace exactly as it does for gab_lookup: the type is
// found in the module that unit declared, or in the root namespace when it
// declared none.
//
// Unlike symbols, types live in one VM-wide registry under a qualified name —
// splitting the registry per module would give each one its own 'int' and
// break the pointer identity the type system compares by. The handle is
// therefore a convenience over that name, not a separate lookup path.
const GabType *gab_find_type(GabVM *vm, GabModule *mod, const char *name);

// As gab_find_type, naming the module directly. NULL means the root namespace.
const GabType *gab_find_type_in(GabVM *vm, const char *module, const char *name);

size_t gab_type_size(const GabType *type);
size_t gab_type_align(const GabType *type);

// Returns false when there is no such field, which is why this reports through
// an out-parameter: a field at offset 0 is otherwise indistinguishable.
bool gab_field_offset(const GabType *type, const char *field, size_t *out_offset);

// --- Calling into a script -------------------------------------------------

// Resolves a name to a callable handle once, so that calling it every frame
// costs no lookup. Returns NULL if the name is missing or names something that
// is not a function.
//
// 'mod' selects the namespace: the name is looked up in the module that unit
// declared, or in the root namespace when the unit declared none or 'mod' is
// NULL. It is honoured, which it previously was not — asking one unit for
// another's symbol used to return that other symbol rather than a miss.
//
// A unit is not itself a namespace, since several may declare the same module.
// Passing any unit of a module therefore reaches all of it.
GabFunc *gab_lookup(GabVM *vm, GabModule *mod, const char *name, GabError *err);

// As gab_lookup, naming the module directly, for a host that has the name but
// not the unit handle. NULL means the root namespace.
GabFunc *gab_lookup_in(GabVM *vm, const char *module, const char *name, GabError *err);

int gab_func_arity(const GabFunc *fn);

void gab_func_free(GabFunc *fn);

// Arguments are written into the handle's own buffer, not the live stack:
// there is no frame yet when these run, and a call the host abandons halfway
// through leaves nothing behind. Each setter checks the index and the callee's
// declared parameter type; a mismatch is remembered and reported by the next
// gab_call rather than corrupting the frame.
//
// There are no varargs by design. C would promote a float to a double, and the
// VM would read eight bytes where the script declared four.
void gab_arg_int(GabVM *vm, GabFunc *fn, int index, int32_t value);
void gab_arg_float(GabVM *vm, GabFunc *fn, int index, float value);
void gab_arg_bool(GabVM *vm, GabFunc *fn, int index, bool value);

// 'size' is checked against the parameter's declared type. Struct layout is
// the one place a host can get the ABI wrong, so it is checked, not trusted.
void gab_arg_struct(GabVM *vm, GabFunc *fn, int index, const void *data, size_t size);

// Calls the function with whatever the setters left in its argument buffer.
// 'ret' may be NULL for a function that returns nothing; otherwise it receives
// the return type's size in bytes, so a struct return works the same way.
GabStatus gab_call(GabVM *vm, GabFunc *fn, void *ret, GabError *err);

#endif
