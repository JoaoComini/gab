#ifndef GAB_H
#define GAB_H

// The only header a host includes. Everything here is opaque on purpose: a
// host embedding Gab should never need to know what a Type, a Symbol, or a
// Value is, and nothing in this file drags those definitions in.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GabVM GabVM;
typedef struct GabScript GabScript;
typedef struct GabFunc GabFunc;
typedef struct GabType GabType;

typedef enum {
    GAB_OK,
    GAB_ERR_COMPILE,
    GAB_ERR_RUNTIME,
    GAB_ERR_ARG,

    // The function this handle was looked up for has been recompiled, and its
    // signature is no longer the one the handle was built against. Look it up
    // again to get a handle for the new signature. A reload that leaves the
    // signature alone does not invalidate anything: the handle goes on calling
    // the new body, which is the point of reloading.
    GAB_ERR_STALE,
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

// Compiles without running, so a script compiles once and its functions are
// called every frame. Returns NULL on failure and fills 'err' if it is
// non-NULL. Nothing is printed: reporting is the host's business.
//
// 'name' names the source for diagnostics only. The namespace a unit declares
// comes from its own 'module' directive, and is a name the host passes to
// gab_lookup and gab_find_type — not something this handle carries.
GabScript *gab_compile(GabVM *vm, const char *name, const char *src, GabError *err);

// Runs the script's top level. Safe to call repeatedly.
GabStatus gab_run(GabVM *vm, GabScript *script, GabError *err);

// Frees the script and its top-level chunk. Everything the compile declared —
// symbols, types, and function prototypes — belongs to the VM and outlives
// this: a prototype index is baked into OP_CALL operands, so it cannot be
// reclaimed per script. Unloading is therefore not supported, and a GabFunc
// looked up from this script keeps working after it is freed.
void gab_script_free(GabVM *vm, GabScript *script);

// --- Types and layout ------------------------------------------------------

// A script struct's layout is the C layout, which is the whole zero-
// marshalling story: these let a host check that against its own sizeof and
// offsetof rather than trusting it.
//
// 'module' is the namespace to look in, as written in a unit's 'module'
// directive; NULL is the root namespace. Types resolve exactly as symbols do:
// the module's own are found first and the root namespace after, so a module's
// 'Config' shadows a root-level one and 'int' needs no import. An unknown
// module is a miss rather than a silent fallback to the root.
const GabType *gab_find_type(GabVM *vm, const char *module, const char *name);

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
// 'module' is the namespace, exactly as for gab_find_type; NULL is the root.
// A module is a name rather than a handle because several units may declare
// the same one: the namespace is the module, not the unit that compiled it.
GabFunc *gab_lookup(GabVM *vm, const char *module, const char *name, GabError *err);

int gab_func_arity(const GabFunc *fn);

// Optional. The VM owns every handle it hands out and releases whatever is
// left when it is freed, so forgetting this leaks nothing — it only reclaims a
// handle sooner, which is worth doing for one a host is discarding rather than
// keeping for the life of the VM.
//
// A handle must not be used after its VM is freed. It reads symbols the VM
// owns, so it dies with the VM; this is the one lifetime rule a host has to
// keep, and it is not checked.
void gab_func_free(GabFunc *fn);

// Arguments are written into the handle's own buffer, not the live stack:
// there is no frame yet when these run, and a call the host abandons halfway
// through leaves nothing behind.
//
// Each returns false if it could not do what it was asked — the index is out
// of range, the parameter is not declared that type, or a struct's size does
// not match. A host that wants to know, checks; a host that does not can
// ignore every one of them, because a setter that failed leaves its parameter
// unset and gab_call refuses a call with an unset parameter. Either way the
// frame is never built from a bad argument.
//
// There are no varargs by design. C would promote a float to a double, and the
// VM would read eight bytes where the script declared four.
bool gab_arg_int(GabVM *vm, GabFunc *fn, int index, int32_t value);
bool gab_arg_float(GabVM *vm, GabFunc *fn, int index, float value);
bool gab_arg_bool(GabVM *vm, GabFunc *fn, int index, bool value);

// 'size' is checked against the parameter's declared type. Struct layout is
// the one place a host can get the ABI wrong, so it is checked, not trusted.
bool gab_arg_struct(GabVM *vm, GabFunc *fn, int index, const void *data, size_t size);

// Calls the function with whatever the setters left in its argument buffer.
// 'ret' may be NULL for a function that returns nothing; otherwise it receives
// the return type's size in bytes, so a struct return works the same way.
GabStatus gab_call(GabVM *vm, GabFunc *fn, void *ret, GabError *err);

#endif
