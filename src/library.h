#ifndef GAB_LIBRARY_H
#define GAB_LIBRARY_H

#include "type/type.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <stddef.h>

typedef struct GabLibrary {
    VM *vm;
    Scope *scope;
    const char *module;
    bool is_prelude;
} GabLibrary;

GabLibrary library_open(VM *vm, const char *module, bool is_prelude);

void library_extern(GabLibrary *lib, const char *type, const char *name, GabExternFn body);

void library_declare_source(GabLibrary *lib, const char *source);

typedef struct LibraryTypeSpec {
    const char *name;
    size_t param_count;

    const TypeFieldSpec *fields;
    size_t field_count;

    const Type *derefs_to;
    const LentPart *lent_parts;
    size_t lent_part_count;
} LibraryTypeSpec;

const TypeDecl *library_type(GabLibrary *lib, const LibraryTypeSpec *spec);

void library_method(GabLibrary *lib, const Type *declared_on, const Type *receiver, const char *name,
                    GabExternFn body, const Type *return_type, const Type *const *params, size_t param_count);

void library_static(GabLibrary *lib, const Type *declared_on, const char *name, GabExternFn body,
                    const Type *return_type, const Type *const *params, size_t param_count);

#endif
