#ifndef GAB_H
#define GAB_H

// The only header a host includes. Everything here is opaque on purpose: a
// host embedding Gab should never need to know what a Type, a Symbol, or a
// stack slot is, and nothing in this file drags those definitions in.

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
// 'name' identifies the unit in diagnostics and nothing else. It does not scope
// what the unit declares, and loading is not idempotent: a declaration is a
// one-time act, so a second unit declaring a name the first one did collides
// and fails to compile, whatever either unit is called.
//
// The namespace a unit declares into is not this name: it comes from the unit's
// own 'module' directive, and is what the host passes to gab_lookup and
// gab_find_type. Two units may name the same module, and one unit's name is
// never the other's.
bool gab_load(GabVM *vm, const char *name, const char *src, GabError *err);

// --- Extern functions ------------------------------------------------------

// A function the host defines and a script calls. The script declares its
// signature with 'extern func f(x: int): int;' and states the types once; the
// host supplies only the body, which is bound to that declaration by name.
//
// The body receives the frame it was called with. Arguments are read by index
// through the gab_arg_get_* accessors and the result is written with one of the
// gab_return_* ones, both addressing the frame's slots in place — the same
// zero-marshalling layout a struct crossing the other way already uses.
typedef struct GabArgs GabArgs;
typedef void (*GabExternFn)(GabArgs *args);

// Binds a host body to the name a script declares 'extern'. 'module' is the
// namespace, exactly as for gab_lookup, and required: every unit names a
// module, so every extern belongs to one.
//
// Registration must happen before the script that declares the name is loaded:
// binding is resolved at load, so that an extern nothing supplies is a load
// failure naming the function rather than a trap the first time that branch
// runs. Returns false and fills 'err' if the VM or name is missing, or the same
// name is already bound in that module.
//
// The binding outlives any one load, so a host registers its externs once at
// startup and loads scripts against them.
bool gab_extern(GabVM *vm, const char *module, const char *name, GabExternFn fn, GabError *err);

// Reading an extern's arguments. The index counts declared parameters from
// zero, and a receiver is parameter zero for a method.
//
// The type must be the one the script declared, and the index must name a
// parameter the declaration has: a slot carries no tag, so reading an int as a
// float reinterprets the bytes rather than converting them.
//
// Neither is reported back. An extern declares its signature in script and
// implements it in C, so an access disagreeing with that signature is a bug in
// one of the two halves and has no recovery a host could be handed — it trips
// an assertion instead, and a zero returned in its place would let the misread
// travel on as data. A release build compiles the assertion out and performs
// the access as written.
int32_t gab_arg_get_int(GabArgs *args, int index);
float gab_arg_get_float(GabArgs *args, int index);
bool gab_arg_get_bool(GabArgs *args, int index);

// Copies a struct argument out into the host's own storage. 'size' is what the
// host expects, and must be what the parameter occupies — the one place an
// extern's ABI can be got wrong.
void gab_arg_get_struct(GabArgs *args, int index, void *out, size_t size);

// The address a pointer argument holds, for a 'box T' or 'ref T' parameter. The
// object belongs to the caller for the duration of the call: an extern reads
// and writes through it and frees nothing.

// A string argument's characters and their count. Borrowed for the call: the
// characters belong to whoever allocated them -- the unit's arena for a
// literal, a script slot for an owning string -- so an extern reads them and
// frees nothing. Not NUL-terminated in general, since a '\0' is an ordinary
// character, so the length is what says where the string ends.
//
// A struct field a host declares is a 'ref str': the host allocated those
// characters and goes on owning them, and the script frees nothing it was lent.
// The host must outlive the script's use of them, as for every borrow it hands
// in.
const char *gab_arg_get_string(GabArgs *args, int index, int32_t *out_length);
void *gab_arg_get_pointer(GabArgs *args, int index);

// Writing an extern's result. A function declaring no return type calls none
// of these; calling one anyway writes a slot the caller will not read.
void gab_return_int(GabArgs *args, int32_t value);
void gab_return_float(GabArgs *args, float value);
void gab_return_bool(GabArgs *args, bool value);
void gab_return_struct(GabArgs *args, const void *data, size_t size);
void gab_return_pointer(GabArgs *args, void *pointer);

// Fails the run from inside an extern. The message is copied, and reaches the
// host that started the run as a GAB_ERR_RUNTIME with this text.
//
// The extern still returns normally: unwinding happens at the boundary, once
// the body is done, because a C function cannot jump out of the interpreter
// without leaving the frames it crossed unreleased. Nothing the body writes
// after this call is read.
void gab_error(GabArgs *args, const char *message);

// --- Types and layout ------------------------------------------------------

// A script struct's layout is the C layout, which is the whole zero-
// marshalling story: these let a host check that against its own sizeof and
// offsetof rather than trusting it.
//
// 'module' is the namespace to look in, as written in a unit's 'module'
// directive, and required: every unit names one. Types resolve exactly as
// symbols do, and only in the module named -- nothing falls through to another
// module, and an unknown module is a miss. Only the builtin types are visible
// from everywhere.
const GabType *gab_find_type(GabVM *vm, const char *module, const char *name);

// How a value of the type sits in memory. Asked of the VM because that is what
// owns the layout: a type is interned once, while how wide it is follows from
// what its parts are laid out as, and the two are kept apart so that neither
// can disagree with the other.
size_t gab_type_size(GabVM *vm, const GabType *type);
size_t gab_type_align(GabVM *vm, const GabType *type);

// Returns false when there is no such field, which is why this reports through
// an out-parameter: a field at offset 0 is otherwise indistinguishable.
bool gab_field_offset(GabVM *vm, const GabType *type, const char *field, size_t *out_offset);

// --- Calling into a script -------------------------------------------------

// Resolves a name to a callable handle once, so that calling it every frame
// costs no lookup. Returns NULL if the name is missing or names something that
// is not a function.
//
// 'module' is the namespace, exactly as for gab_find_type, and required. A
// module is a name rather than a handle because several units may declare the
// same one: the namespace is the module, not the unit that compiled it.
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

// Stages a pointer argument. 'inner' is what the parameter must point at, as
// returned by gab_find_type — the inner is checked rather than trusted, since
// a 'box Player' where 'box Enemy' was declared is exactly the mistake the layout
// story cannot survive.
//
// A parameter is a borrow whichever way it is declared: the callee neither
// takes ownership on entry nor frees it on return, so the object must outlive
// the call and the host goes on owning it. What the callee may do with it
// differs — an owning 'box T' parameter may be stored into a field, which hands
// ownership to that object; a 'ref T' one may not.
//
// Returns false if the index is out of range, the parameter is not a pointer,
// or its inner is not this type.
bool gab_arg_pointer(GabCall *call, int index, void *pointer, const GabType *inner);

// Calls the function with whatever the setters left in the call's buffer.
// 'ret' may be NULL for a function that returns nothing; otherwise it receives
// the return type's size in bytes, so a struct return works the same way.
//
// A function returning 'box T' hands over ownership, and there is no host call
// that frees one: what a script allocated, only a script frees today. A 'ref T'
// return owns nothing and names something the script still holds.
GabStatus gab_call(GabVM *vm, GabCall *call, void *ret, GabError *err);

#endif
