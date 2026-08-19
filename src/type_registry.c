#include "type_registry.h"

#include "arena.h"
#include "string/string.h"
#include "type.h"

#include <assert.h>
#include <stdlib.h>

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

    // Poison type. Deliberately never given a name in any scope: no script can
    // name it, it only arises from a failed resolution.
    registry->builtins.error_type = register_builtin(registry, TYPE_ERROR, "<error>", 0, 1);
}

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->pointers = pointer_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->weak_pointers =
        pointer_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    registry->strings = strings;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    pointer_map_destroy(registry->pointers);
    pointer_map_destroy(registry->weak_pointers);
}

Type *type_registry_pointer_to(TypeRegistry *registry, Type *pointee) {
    return type_registry_pointer_to_kind(registry, pointee, false);
}

Type *type_registry_pointer_to_kind(TypeRegistry *registry, Type *pointee, bool weak) {
    // Two maps rather than a composite key: 'weak *T' and '*T' are different
    // types, and the whole type system compares by pointer identity, so they
    // must never collide in one table.
    PointerMap *map = weak ? registry->weak_pointers : registry->pointers;

    Type **existing = pointer_map_lookup(map, pointee);
    if (existing) {
        return *existing;
    }

    // No name: a pointer type is structural, so its printable form is derived
    // from the pointee when a diagnostic asks. See Type::name.
    Type *type = type_create(registry->arena, TYPE_POINTER, NULL);

    // Always a raw address to the payload, so a stack pointer and a heap one
    // are byte-identical; only the resolver knows which is which. A weak
    // pointer is the same address too — what differs is the count it touches.
    type->size = sizeof(void *);
    type->alignment = _Alignof(void *);
    type->pointee = pointee;
    type->is_weak = weak;

    pointer_map_insert(map, pointee, type);

    return type;
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
