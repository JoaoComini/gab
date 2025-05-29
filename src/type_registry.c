#include "type_registry.h"

#include "arena.h"
#include "string/string.h"
#include "type.h"

#include <assert.h>
#include <stdlib.h>

void type_registry_register_builtins(TypeRegistry *registry) {
    registry->builtins.int_type = type_create(registry->arena, TYPE_INT, string_from_cstr("int"));
    registry->builtins.float_type = type_create(registry->arena, TYPE_FLOAT, string_from_cstr("float"));
    registry->builtins.bool_type = type_create(registry->arena, TYPE_BOOL, string_from_cstr("bool"));

    type_map_insert(registry->map, registry->builtins.int_type->name, registry->builtins.int_type);
    type_map_insert(registry->map, registry->builtins.float_type->name, registry->builtins.float_type);
    type_map_insert(registry->map, registry->builtins.bool_type->name, registry->builtins.bool_type);
}

TypeRegistry *type_registry_create(Arena *arena) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->map = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) { type_map_destroy(registry->map); }

Type *type_registry_get(TypeRegistry *registry, String *name) {
    return *type_map_lookup(registry->map, name);
}

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
}
