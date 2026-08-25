#include "type_registry.h"

#include "arena.h"
#include "object.h"
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

    // Two fields, so its size and alignment come from the layout the VM reads
    // rather than from a number repeated here.
    registry->builtins.string_type =
        register_builtin(registry, TYPE_STRING, "string", sizeof(GabStringValue), _Alignof(GabStringValue));

    // Same layout and same name: 'ref' is spelled in the type spec rather than
    // being a type a script can name, so this one is never registered under a
    // name of its own.
    registry->builtins.ref_string_type =
        type_create(registry->arena, TYPE_STRING, registry->builtins.string_type->name);
    registry->builtins.ref_string_type->size = sizeof(GabStringValue);
    registry->builtins.ref_string_type->alignment = _Alignof(GabStringValue);
    registry->builtins.ref_string_type->is_ref = true;
    registry->builtins.ref_string_type->owner = registry->builtins.string_type;

    // Characters own nothing, so this is a plain sized payload: TYPE_UNKNOWN
    // reaches neither the string branch of the free walk nor the field loop.
    registry->builtins.characters_type = type_create(registry->arena, TYPE_UNKNOWN, NULL);

    // Poison type. Deliberately never given a name in any scope: no script can
    // name it, it only arises from a failed resolution.
    registry->builtins.error_type = register_builtin(registry, TYPE_ERROR, "<error>", 0, 1);
}

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->indirects = indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->ref_indirects =
        indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    registry->strings = strings;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    indirect_map_destroy(registry->indirects);
    indirect_map_destroy(registry->ref_indirects);
}

Type *type_registry_indirect_to(TypeRegistry *registry, Type *inner) {
    return type_registry_indirect_to_kind(registry, inner, false);
}

Type *type_registry_indirect_to_kind(TypeRegistry *registry, Type *inner, bool is_ref) {
    // Two maps rather than a composite key: 'ref T' and 'box T' are different
    // types, and the whole type system compares by pointer identity, so they
    // must never collide in one table.
    IndirectMap *map = is_ref ? registry->ref_indirects : registry->indirects;

    Type **existing = indirect_map_lookup(map, inner);
    if (existing) {
        return *existing;
    }

    // No name: a pointer type is structural, so its printable form is derived
    // from the inner when a diagnostic asks. See Type::name.
    Type *type = type_create(registry->arena, TYPE_INDIRECT, NULL);

    // Always a raw address to the payload, so a stack pointer and a heap one
    // are byte-identical; only the resolver knows which is which. A 'ref T' is
    // the same address too — what differs is who frees the inner.
    type->size = sizeof(void *);
    type->alignment = _Alignof(void *);
    type->inner = inner;
    type->is_ref = is_ref;

    indirect_map_insert(map, inner, type);

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
    case TYPE_STRING:
        return registry->builtins.string_type;
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
    abort();
}
