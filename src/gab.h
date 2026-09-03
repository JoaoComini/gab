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

typedef struct GabCtx GabCtx;
typedef void (*GabExternFn)(GabCtx *ctx);

bool gab_extern(GabVM *vm, const char *module, const char *type, const char *name, GabExternFn fn,
                GabError *err);

int32_t gab_arg_get_int(GabCtx *ctx, int index);
float gab_arg_get_float(GabCtx *ctx, int index);
bool gab_arg_get_bool(GabCtx *ctx, int index);

void gab_arg_get_struct(GabCtx *ctx, int index, void *out, size_t size);

const char *gab_arg_get_string(GabCtx *ctx, int index, int32_t *out_length);
void *gab_arg_get_pointer(GabCtx *ctx, int index);

void gab_return_int(GabCtx *ctx, int32_t value);
void gab_return_float(GabCtx *ctx, float value);
void gab_return_bool(GabCtx *ctx, bool value);
void gab_return_struct(GabCtx *ctx, const void *data, size_t size);
void gab_return_pointer(GabCtx *ctx, void *pointer);

void gab_ctx_fail(GabCtx *ctx, const char *message);

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

/* What the declaration's type parameters were instantiated with, in order. One body serves every
 * specialization, so these are how it tells them apart. */
size_t gab_ctx_type_count(GabCtx *ctx);
GabTypeKind gab_ctx_type_kind(GabCtx *ctx, size_t index);
size_t gab_ctx_type_size(GabCtx *ctx, size_t index);

/* The length and element size of an array parameter. Both are zero when that parameter is not an
 * array. */
int32_t gab_ctx_array_length(GabCtx *ctx, int index);
size_t gab_ctx_array_stride(GabCtx *ctx, int index);

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
