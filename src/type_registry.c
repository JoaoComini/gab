#include "type_registry.h"

#include "string_ref.h"
#include "type.h"

#include <assert.h>

const Type INT_TYPE = (Type){.kind = TYPE_INT, .name = (StringRef){.data = "int", .length = 3}};
const Type FLOAT_TYPE = (Type){.kind = TYPE_FLOAT, .name = (StringRef){.data = "float", .length = 5}};
const Type BOOL_TYPE = (Type){.kind = TYPE_BOOL, .name = (StringRef){.data = "bool", .length = 4}};

void type_registry_register_builtin(TypeRegistry *registry) {
    type_registry_insert(registry, INT_TYPE.name, INT_TYPE);
    type_registry_insert(registry, FLOAT_TYPE.name, FLOAT_TYPE);
    type_registry_insert(registry, BOOL_TYPE.name, BOOL_TYPE);
}

Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind kind) {
    switch (kind) {
    case TYPE_INT:
        return type_registry_lookup(registry, string_ref_create("int"));
    case TYPE_FLOAT:
        return type_registry_lookup(registry, string_ref_create("float"));
    case TYPE_BOOL:
        return type_registry_lookup(registry, string_ref_create("bool"));
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
}
