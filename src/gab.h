#ifndef GAB_H
#define GAB_H

// The only header a host includes. Everything here is opaque on purpose: a
// host embedding Gab should never need to know what a Type, a Symbol, or a
// Value is, and nothing in this file drags those definitions in.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GabVM GabVM;
typedef struct GabFunc GabFunc;
typedef struct GabType GabType;
typedef struct GabCall GabCall;

typedef enum {
    GAB_OK,
    GAB_ERR_COMPILE,
    GAB_ERR_RUNTIME,
    GAB_ERR_ARG,

    // The function this call was prepared for has been recompiled, and its
    // signature is no longer the one the call was built against. Its staged
    // arguments described the old signature, so they are gone: initialise the
    // call again to stage them afresh. A reload that leaves the signature alone
    // does not invalidate anything, and the call goes on reaching the new body,
    // which is the point of reloading.
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

// --- Loading ---------------------------------------------------------------

// Compiles a unit and runs its top level, which is what declares its functions
// and types to the VM and initialises whatever the unit sets up. Returns false
// on failure and fills 'err' if it is non-NULL. Nothing is printed: reporting
// is the host's business.
//
// There is nothing to free, and nothing to hold. The VM owns the compiled unit
// and releases it with itself, so loading is a statement rather than a resource
// a host has to keep track of.
//
// 'name' identifies the unit. It names the source in diagnostics, and it is the
// key a reload replaces: loading a name already loaded compiles the new source
// over the old one and frees what the old one held, so a host that recompiles a
// file whenever it changes on disk accumulates nothing. Declarations are
// replaced the same way, which is what makes this hot reload — a GabFunc looked
// up before a reload goes on working, calling the new body.
//
// The namespace a unit declares into is not this name: it comes from the unit's
// own 'module' directive, and is what the host passes to gab_lookup and
// gab_find_type. Two units may name the same module, and one unit's name is
// never the other's.
bool gab_load(GabVM *vm, const char *name, const char *src, GabError *err);


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
//
// There is nothing to free. The VM owns every handle it hands out and releases
// them with itself, so a host looks a function up and then forgets about its
// lifetime — the handles a program accumulates are bounded by the functions it
// calls, which is one lookup each.
//
// The one rule, and it is not checked: a handle must not be used after its VM
// is freed. It reads symbols the VM owns, so it dies with the VM.
GabFunc *gab_lookup(GabVM *vm, const char *module, const char *name, GabError *err);

int gab_func_arity(const GabFunc *fn);

// --- Staging arguments -----------------------------------------------------

// A GabCall is one caller's staged arguments for one function. A handle is
// shared and immutable; the arguments being built up for a call are not, so
// they live here — which is what lets two parts of a host call the same
// function without one overwriting what the other staged.
//
// Sized for the function's signature, so this allocates. It is the one thing
// in this API a host frees: gab_call_free when the caller is done with it.
// Returns NULL if the function is NULL or memory ran out.
GabCall *gab_call_init(GabFunc *fn, GabError *err);

// After GAB_ERR_STALE: resizes the call for the signature the function now has
// and clears what it staged, so the host stages afresh and calls again. The
// GabCall keeps its address, so whatever the host stored it in goes on pointing
// at a live call.
//
// Returns false only if memory ran out, leaving the call as it was — still
// stale, still refusing, still the host's to free.
bool gab_call_restage(GabCall *call, GabError *err);

void gab_call_free(GabCall *call);

// Arguments are written into the call's own buffer, not the live stack: there
// is no frame yet when these run, and a call the host abandons halfway through
// leaves nothing behind. They persist across calls on purpose, so a host
// holding one argument constant sets it once and re-sets only what changes.
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
bool gab_arg_int(GabCall *call, int index, int32_t value);
bool gab_arg_float(GabCall *call, int index, float value);
bool gab_arg_bool(GabCall *call, int index, bool value);

// 'size' is checked against the parameter's declared type. Struct layout is
// the one place a host can get the ABI wrong, so it is checked, not trusted.
bool gab_arg_struct(GabCall *call, int index, const void *data, size_t size);

// Calls the function with whatever the setters left in the call's buffer.
// 'ret' may be NULL for a function that returns nothing; otherwise it receives
// the return type's size in bytes, so a struct return works the same way.
GabStatus gab_call(GabVM *vm, GabCall *call, void *ret, GabError *err);

#endif
