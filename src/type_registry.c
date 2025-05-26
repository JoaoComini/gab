#include "type_registry.h"

#include "type.h"

#include <assert.h>

static Type *INT_TYPE = NULL;
static Type *FLOAT_TYPE = NULL;
static Type *BOOL_TYPE = NULL;

void type_registry_register_builtin(TypeRegistry *registry) {
    Type int_type = (Type){.kind = TYPE_INT, .name = string_from_cstr("int")};
    Type float_type = (Type){.kind = TYPE_FLOAT, .name = string_from_cstr("float")};
    Type bool_type = (Type){.kind = TYPE_BOOL, .name = string_from_cstr("bool")};

    INT_TYPE = type_registry_insert(registry, int_type.name, int_type);
    FLOAT_TYPE = type_registry_insert(registry, float_type.name, float_type);
    BOOL_TYPE = type_registry_insert(registry, bool_type.name, bool_type);
}

Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind kind) {
    switch (kind) {
    case TYPE_INT:
        return INT_TYPE;
    case TYPE_FLOAT:
        return FLOAT_TYPE;
    case TYPE_BOOL:
        return BOOL_TYPE;
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
}
