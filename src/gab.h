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

typedef struct GabArgs GabArgs;
typedef void (*GabExternFn)(GabArgs *args);

bool gab_extern(GabVM *vm, const char *module, const char *type, const char *name, GabExternFn fn,
                GabError *err);

int32_t gab_arg_get_int(GabArgs *args, int index);
float gab_arg_get_float(GabArgs *args, int index);
bool gab_arg_get_bool(GabArgs *args, int index);

void gab_arg_get_struct(GabArgs *args, int index, void *out, size_t size);

const char *gab_arg_get_string(GabArgs *args, int index, int32_t *out_length);
void *gab_arg_get_pointer(GabArgs *args, int index);

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
