#include "type_registry.h"

#include "arena.h"
#include "string/string.h"
#include "type.h"

#include <assert.h>
#include <stdlib.h>

void type_registry_register_builtins(TypeRegistry *registry) {
    StringPool *strings = registry->strings;

    registry->builtins.int_type = type_create(registry->arena, TYPE_INT, string_from_cstr(strings, "int"));
    registry->builtins.float_type =
        type_create(registry->arena, TYPE_FLOAT, string_from_cstr(strings, "float"));
    registry->builtins.bool_type = type_create(registry->arena, TYPE_BOOL, string_from_cstr(strings, "bool"));

    // Poison type. Deliberately not inserted into the name map: no script can
    // name it, it only arises from a failed resolution.
    registry->builtins.error_type =
        type_create(registry->arena, TYPE_ERROR, string_from_cstr(strings, "<error>"));

    type_map_insert(registry->map, registry->builtins.int_type->name, registry->builtins.int_type);
    type_map_insert(registry->map, registry->builtins.float_type->name, registry->builtins.float_type);
    type_map_insert(registry->map, registry->builtins.bool_type->name, registry->builtins.bool_type);
}

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->map = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    registry->strings = strings;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) { type_map_destroy(registry->map); }

Type *type_registry_get(TypeRegistry *registry, String *name) {
    Type **type = type_map_lookup(registry->map, name);

    return type ? *type : NULL;
}

Type *type_registry_error_type(TypeRegistry *registry) { return registry->builtins.error_type; }

Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind kind) {
    switch (kind) {
    case TYPE_INT:
        return registry->builtins.int_type;
    case TYPE_FLOAT:
        return registry->builtins.float_type;
    case TYPE_BOOL:
        return registry->builtins.bool_type;
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
    abort();
}
