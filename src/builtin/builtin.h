#ifndef GAB_BUILTIN_H
#define GAB_BUILTIN_H

#include "type/type.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <stddef.h>

void builtin_register_all(VM *vm);

typedef struct BuiltinTypeSpec {
    const char *name;
    size_t param_count;

    const TypeFieldSpec *fields;
    size_t field_count;

    const Type *derefs_to;
    const LentPart *lent_parts;
    size_t lent_part_count;
} BuiltinTypeSpec;

const TypeDef *builtin_declare(VM *vm, const BuiltinTypeSpec *spec);

void builtin_register_method(VM *vm, const Type *declared_on, const Type *receiver, const char *name,
                             GabExternFn body, const Type *return_type, const Type *const *params,
                             size_t param_count);

void builtin_declare_method(VM *vm, const TypeDef *declared_on, const char *name, GabExternFn body,
                            const Type *receiver, const Type *result, const Type *const *params,
                            size_t param_count);

void builtin_register_static(VM *vm, const Type *declared_on, const char *name, GabExternFn body,
                             const Type *return_type, const Type *const *params, size_t param_count);

void builtin_register_string(VM *vm);

void builtin_register_vec(VM *vm);

#endif
