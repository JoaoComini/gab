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

// Frees the module and its top-level chunk. The function prototypes the module
// added stay for the VM's life: a prototype index is baked into OP_CALL
// operands, so they cannot be reclaimed per module until modules own their
// own scopes. Unloading one module of several is therefore not yet supported.
void gab_module_free(GabVM *vm, GabModule *mod);

// --- Types and layout ------------------------------------------------------

// A script struct's layout is the C layout, which is the whole zero-
// marshalling story: these let a host check that against its own sizeof and
// offsetof rather than trusting it.
const GabType *gab_find_type(GabVM *vm, GabModule *mod, const char *name);
size_t gab_type_size(const GabType *type);
size_t gab_type_align(const GabType *type);

// Returns false when there is no such field, which is why this reports through
// an out-parameter: a field at offset 0 is otherwise indistinguishable.
bool gab_field_offset(const GabType *type, const char *field, size_t *out_offset);

// --- Calling into a script -------------------------------------------------

// Resolves a name to a callable handle once, so that calling it every frame
// costs no lookup. Returns NULL if the name is missing or names something that
// is not a function.
GabFunc *gab_lookup(GabVM *vm, GabModule *mod, const char *name, GabError *err);
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
