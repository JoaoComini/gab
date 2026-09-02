#ifndef GAB_H
#define GAB_H

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

typedef struct {
    char message[256];
    int line;
    int column;
} GabError;

GabVM *gab_vm_new(void);
void gab_vm_free(GabVM *vm);

bool gab_load(GabVM *vm, const char *name, const char *src, GabError *err);

/* The values match the VM's own type kinds and are stable. */
typedef enum {
    GAB_TYPE_INT,
    GAB_TYPE_FLOAT,
    GAB_TYPE_BOOL,
    GAB_TYPE_BYTE,
    GAB_TYPE_PTR,
    GAB_TYPE_STR,
    GAB_TYPE_ARRAY,
    GAB_TYPE_STRUCT,
    GAB_TYPE_BOX,
    GAB_TYPE_REF,
    GAB_TYPE_BLOCK,
} GabTypeKind;

typedef struct GabArgs GabArgs;
typedef void (*GabExternFn)(GabArgs *args);

bool gab_extern(GabVM *vm, const char *module, const char *type, const char *name, GabExternFn fn,
                GabError *err);

/* What the VM knows about one call, handed to every bound symbol as its first parameter. It is valid
 * only for the duration of that call. */
typedef struct GabCtx GabCtx;

/* A string the script owns: the same layout the VM holds, so a body returns one by value. */
typedef struct {
    void *data;
    int32_t capacity;
    int32_t length;
} GabStr;

/* Bind an extern to a C symbol called directly, with no shim. The symbol takes a GabCtx * ahead of
 * the parameters its declaration names, and the declaration's types describe the rest of the call,
 * so a mismatch with the symbol's real signature is undefined at the call. */
bool gab_extern_c(GabVM *vm, const char *module, const char *type, const char *name, void *symbol,
                  GabError *err);

/* Fail the running call with a message. The C body should return promptly; its return value is
 * discarded, and the script sees a runtime error rather than a result. */
void gab_ctx_fail(GabCtx *ctx, const char *message);

/* What the declaration's type parameters were instantiated with, in order. One symbol serves every
 * specialization, so these are how a body tells them apart. */
size_t gab_ctx_type_count(GabCtx *ctx);
GabTypeKind gab_ctx_type_kind(GabCtx *ctx, size_t index);
size_t gab_ctx_type_size(GabCtx *ctx, size_t index);

/* Where the call's return value goes, for a body whose declaration returns a type its specialization
 * chose. Such a body is passed the address as its second argument, ahead of the parameters its
 * declaration names, and writes gab_ctx_type_size bytes there rather than returning a value. */
void *gab_ctx_return(GabCtx *ctx);

/* Allocate a box of the declared return type, owned by the script once the body returns it. Returns
 * NULL and fails the call when there is no memory, so a body that returns the result unchecked
 * traps rather than hands back nothing. */
void *gab_box(GabCtx *ctx);

/* Copy a string into memory the script owns once the body returns it. The result is the value the
 * body returns, not a slot written behind its back. On failure the call is failed and the result is
 * empty, so a body that returns it unchecked traps rather than hands back memory it does not own. */
GabStr gab_str_copy(GabCtx *ctx, const char *data, int32_t length);

int32_t gab_arg_get_int(GabArgs *args, int index);
float gab_arg_get_float(GabArgs *args, int index);
bool gab_arg_get_bool(GabArgs *args, int index);

void gab_arg_get_struct(GabArgs *args, int index, void *out, size_t size);

const char *gab_arg_get_string(GabArgs *args, int index, int32_t *out_length);
void *gab_arg_get_pointer(GabArgs *args, int index);

/* Free what an owning parameter holds. A C body is given ownership of such a parameter and nothing
 * else will free it, so a body that neither drops it nor hands it back leaks. Dropping a parameter
 * whose value the body has returned, or stored where the script can still reach it, frees memory the
 * script still names. */
void gab_drop(GabArgs *args, int index);

/* Free an owning pointer a script passed to a C symbol bound by gab_extern_c, which is handed the
 * pointer alone and so has no GabArgs to name a parameter by index. The pointer must be one the
 * script gave up ownership of, and must not have been freed already. */
void gab_drop_pointer(void *pointer);

void gab_return_int(GabArgs *args, int32_t value);
void gab_return_float(GabArgs *args, float value);
void gab_return_bool(GabArgs *args, bool value);
void gab_return_struct(GabArgs *args, const void *data, size_t size);
void gab_return_pointer(GabArgs *args, void *pointer);

void gab_error(GabArgs *args, const char *message);

const GabType *gab_find_type(GabVM *vm, const char *module, const char *name);

size_t gab_type_size(GabVM *vm, const GabType *type);
size_t gab_type_align(GabVM *vm, const GabType *type);

bool gab_field_offset(GabVM *vm, const GabType *type, const char *field, size_t *out_offset);

GabFunc *gab_lookup(GabVM *vm, const char *module, const char *name, GabError *err);

int gab_func_arity(const GabFunc *fn);

GabCall *gab_call_init(GabFunc *fn, GabError *err);

void gab_call_free(GabCall *call);

bool gab_arg_int(GabCall *call, int index, int32_t value);
bool gab_arg_float(GabCall *call, int index, float value);
bool gab_arg_bool(GabCall *call, int index, bool value);

bool gab_arg_struct(GabCall *call, int index, const void *data, size_t size);

bool gab_arg_pointer(GabCall *call, int index, void *pointer, const GabType *inner);

GabStatus gab_call(GabVM *vm, GabCall *call, void *ret, GabError *err);

#endif
