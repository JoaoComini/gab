#ifndef GAB_H
#define GAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GabVM GabVM;
typedef struct GabFunc GabFunc;
typedef struct GabType GabType;
typedef struct GabCall GabCall;
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
    GAB_TYPE_I32,
    GAB_TYPE_F32,
    GAB_TYPE_BOOL,
    GAB_TYPE_U8,
    GAB_TYPE_PTR,
    GAB_TYPE_STR,
    GAB_TYPE_ARRAY,
    GAB_TYPE_SLICE,
    GAB_TYPE_STRUCT,
    GAB_TYPE_BOX,
    GAB_TYPE_REF,
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

/* ---- Declaring a library ---- */

GabLib *gab_lib_open(GabVM *vm, const char *module, GabError *err);

/* The declarations, without the 'module' line: the module is the one gab_lib_open named. */
bool gab_lib_source(GabLib *lib, const char *source, GabError *err);

void gab_lib_close(GabLib *lib);

#endif
