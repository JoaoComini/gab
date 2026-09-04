#ifndef GAB_H
#define GAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GabVM GabVM;
typedef struct GabFunc GabFunc;
typedef struct GabType GabType;
typedef struct GabCall GabCall;
typedef struct GabCtx GabCtx;
typedef struct GabLib GabLib;

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

/* The values match the VM's own type kinds and are stable. */
typedef enum {
    GAB_TYPE_INT,
    GAB_TYPE_FLOAT,
    GAB_TYPE_BOOL,
    GAB_TYPE_BYTE,
    GAB_TYPE_PTR,
    GAB_TYPE_STR,
    GAB_TYPE_ARRAY,
    GAB_TYPE_SLICE,
    GAB_TYPE_STRUCT,
    GAB_TYPE_BOX,
    GAB_TYPE_REF,
    GAB_TYPE_BLOCK,
} GabTypeKind;

/* ---- The VM, and the host's side of a call into it ---- */

GabVM *gab_vm_new(void);
void gab_vm_free(GabVM *vm);

bool gab_vm_load(GabVM *vm, const char *name, const char *src, GabError *err);

GabFunc *gab_vm_lookup(GabVM *vm, const char *module, const char *name, GabError *err);

const GabType *gab_vm_find_type(GabVM *vm, const char *module, const char *name);

size_t gab_type_size(GabVM *vm, const GabType *type);
size_t gab_type_align(GabVM *vm, const GabType *type);

bool gab_type_field_offset(GabVM *vm, const GabType *type, const char *field, size_t *out_offset);

int gab_func_arity(const GabFunc *fn);

GabCall *gab_call_init(GabFunc *fn, GabError *err);
void gab_call_free(GabCall *call);

bool gab_call_int(GabCall *call, int index, int32_t value);
bool gab_call_float(GabCall *call, int index, float value);
bool gab_call_bool(GabCall *call, int index, bool value);
bool gab_call_struct(GabCall *call, int index, const void *data, size_t size);
bool gab_call_pointer(GabCall *call, int index, void *pointer, const GabType *inner);

GabStatus gab_call(GabVM *vm, GabCall *call, void *ret, GabError *err);

/* ---- Inside a host body ---- */

typedef void (*GabExternFn)(GabCtx *ctx);

int32_t gab_ctx_int(GabCtx *ctx, int index);
float gab_ctx_float(GabCtx *ctx, int index);
bool gab_ctx_bool(GabCtx *ctx, int index);
const char *gab_ctx_string(GabCtx *ctx, int index, int32_t *out_length);
void *gab_ctx_pointer(GabCtx *ctx, int index);
void gab_ctx_struct(GabCtx *ctx, int index, void *out, size_t size);

/* The address a parameter occupies in the frame, for a body whose declaration types it as a
 * parameter the body only knows the size of. */
const void *gab_ctx_address(GabCtx *ctx, int index);

/* The address of the receiver a method was called on. */
void *gab_ctx_self(GabCtx *ctx);

void gab_ctx_return_int(GabCtx *ctx, int32_t value);
void gab_ctx_return_float(GabCtx *ctx, float value);
void gab_ctx_return_bool(GabCtx *ctx, bool value);
void gab_ctx_return_struct(GabCtx *ctx, const void *data, size_t size);
void gab_ctx_return_pointer(GabCtx *ctx, void *pointer);
bool gab_ctx_return_string(GabCtx *ctx, const char *data, int32_t length);

/* What the declaration's type parameters were instantiated with, in order. One body serves every
 * specialization, so these are how it tells them apart. */
size_t gab_ctx_type_count(GabCtx *ctx);
GabTypeKind gab_ctx_type_kind(GabCtx *ctx, size_t index);
size_t gab_ctx_type_size(GabCtx *ctx, size_t index);

/* The length and element size of an array parameter. Both are zero when that parameter is not an
 * array. */
int32_t gab_ctx_array_length(GabCtx *ctx, int index);
size_t gab_ctx_array_stride(GabCtx *ctx, int index);

GabVM *gab_ctx_vm(GabCtx *ctx);

typedef enum {
    GAB_FAIL_RUNTIME,
    GAB_FAIL_OUT_OF_MEMORY,
    GAB_FAIL_BOUNDS,
} GabFailure;

void gab_ctx_fail(GabCtx *ctx, GabFailure failure, const char *message);

/* ---- An owned, growable buffer ---- */

/* The layout a script sees for a host type's storage field, so a host struct embedding one has the
 * same bytes the VM reads. */
typedef struct {
    void *data;
    int32_t capacity;
    int32_t length;
} GabBlock;

bool gab_block_reserve(GabCtx *ctx, GabBlock *block, int32_t extra, size_t stride);

/* ---- Declaring a library ---- */

GabLib *gab_lib_open(GabVM *vm, const char *module, GabError *err);

/* The types a declaration's fields are built from. Valid until the VM is freed. */
const GabType *gab_lib_primitive(GabLib *lib, GabTypeKind kind);
const GabType *gab_lib_param(GabLib *lib, size_t index);
const GabType *gab_lib_block_of(GabLib *lib, const GabType *element);
const GabType *gab_lib_array_of(GabLib *lib, const GabType *element, int32_t length);
const GabType *gab_lib_slice_of(GabLib *lib, const GabType *element);
const GabType *gab_lib_ptr_to(GabLib *lib, const GabType *pointee);

typedef struct {
    const char *name;
    const GabType *type;
} GabFieldSpec;

/* A part of a value that names memory the value does not own, as an offset and size into it. */
typedef struct {
    size_t offset;
    size_t size;
} GabLentPart;

typedef struct {
    const char *name;
    size_t params;

    const GabFieldSpec *fields;
    size_t field_count;

    /* The type '&self' reads as, for a type that is a view over one it owns. */
    const GabType *derefs_to;

    const GabLentPart *lends;
    size_t lend_count;
} GabTypeSpec;

const GabType *gab_lib_type(GabLib *lib, const GabTypeSpec *spec, GabError *err);

/* The declarations, without the 'module' line: the module is the one gab_lib_open named. */
bool gab_lib_source(GabLib *lib, const char *source, GabError *err);

bool gab_lib_bind(GabLib *lib, const char *type, const char *name, GabExternFn body, GabError *err);

void gab_lib_close(GabLib *lib);

/* ---- Binding an extern outside a library ---- */

bool gab_extern(GabVM *vm, const char *module, const char *type, const char *name, GabExternFn fn,
                GabError *err);

#endif
