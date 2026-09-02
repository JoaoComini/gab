#ifndef GAB_LIBRARY_H
#define GAB_LIBRARY_H

#include "type/type.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <stddef.h>

typedef struct Library {
    VM *vm;
    Scope *scope;
    const char *module;
    bool is_prelude;
} Library;

Library library_open(VM *vm, const char *module, bool is_prelude);

void library_extern(Library *lib, const char *type, const char *name, void *symbol);

void library_declare_source(Library *lib, const char *source);

typedef struct LibraryTypeSpec {
    const char *name;
    size_t param_count;

    const TypeFieldSpec *fields;
    size_t field_count;

    const Type *derefs_to;
    const LentPart *lent_parts;
    size_t lent_part_count;
} LibraryTypeSpec;

const TypeDecl *library_type(Library *lib, const LibraryTypeSpec *spec);

#endif
