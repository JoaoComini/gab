#include "type_registry.h"

#include "arena.h"
#include "string/string.h"
#include "type.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Type *register_builtin(TypeRegistry *registry, TypeKind kind, const char *name, size_t size,
                              size_t alignment) {
    Type *type = type_create(registry->arena, kind, string_from_cstr(registry->strings, name));
    type->size = size;
    type->alignment = alignment;

    return type;
}

void type_registry_register_builtins(TypeRegistry *registry) {
    registry->builtins.int_type = register_builtin(registry, TYPE_INT, "int", 4, 4);
    registry->builtins.float_type = register_builtin(registry, TYPE_FLOAT, "float", 4, 4);
    registry->builtins.bool_type = register_builtin(registry, TYPE_BOOL, "bool", 1, 1);

    // Poison type. Deliberately not inserted into the name map: no script can
    // name it, it only arises from a failed resolution.
    registry->builtins.error_type = register_builtin(registry, TYPE_ERROR, "<error>", 0, 1);

    type_map_insert(registry->map, registry->builtins.int_type->name, registry->builtins.int_type);
    type_map_insert(registry->map, registry->builtins.float_type->name, registry->builtins.float_type);
    type_map_insert(registry->map, registry->builtins.bool_type->name, registry->builtins.bool_type);
}

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->map = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->pointers = pointer_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    registry->strings = strings;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    type_map_destroy(registry->map);
    pointer_map_destroy(registry->pointers);
}

Type *type_registry_pointer_to(TypeRegistry *registry, Type *pointee) {
    Type **existing = pointer_map_lookup(registry->pointers, pointee);
    if (existing) {
        return *existing;
    }

    // The name is only ever read by diagnostics, but it is interned like any
    // other so that a Type always has a printable name.
    size_t length = strlen(pointee->name->data) + 2;
    char *name = arena_alloc(registry->arena, length);
    snprintf(name, length, "*%s", pointee->name->data);

    Type *type = type_create(registry->arena, TYPE_POINTER, string_from_cstr(registry->strings, name));

    // Always a raw address to the payload, so a stack pointer and a heap one
    // are byte-identical; only the resolver knows which is which.
    type->size = sizeof(void *);
    type->alignment = _Alignof(void *);
    type->pointee = pointee;

    pointer_map_insert(registry->pointers, pointee, type);

    return type;
}

Type *type_registry_get(TypeRegistry *registry, String *name) {
    Type **type = type_map_lookup(registry->map, name);

    return type ? *type : NULL;
}

bool type_registry_register(TypeRegistry *registry, Type *type) {
    if (type_registry_get(registry, type->name)) {
        return false;
    }

    type_map_insert(registry->map, type->name, type);
    return true;
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
