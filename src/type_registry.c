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

// The 'string' type, laid out from its two fields. 'is_ref' decides only
// whether the field naming the characters owns them: that one bit is the whole
// difference between a string and a borrow of one.
static Type *string_builtin_create(TypeRegistry *registry, bool is_ref) {
    Type *type = type_struct_create(registry->arena, string_from_cstr(registry->strings, "string"), 2);
    type->kind = TYPE_STRING;

    Type *characters = type_registry_indirect_to_kind(registry, registry->builtins.characters_type, is_ref);

    type_add_field(type, string_from_cstr(registry->strings, "data"), characters);
    type_add_field(type, string_from_cstr(registry->strings, "length"), registry->builtins.int_type);
    type_layout_compute(type);
    object_select_drop(type);

    return type;
}

void type_registry_register_builtins(TypeRegistry *registry) {
    registry->builtins.int_type = register_builtin(registry, TYPE_INT, "int", 4, 4);
    registry->builtins.float_type = register_builtin(registry, TYPE_FLOAT, "float", 4, 4);
    registry->builtins.bool_type = register_builtin(registry, TYPE_BOOL, "bool", 1, 1);

    // What a string's characters are a buffer of.
    registry->builtins.byte_type = register_builtin(registry, TYPE_BYTE, "byte", 1, 1);

    registry->builtins.characters_type = type_registry_buffer_of(registry, registry->builtins.byte_type);

    // A struct in its layout and a builtin in its semantics: the fields are
    // where its size, its alignment and what it owns all come from, while
    // comparison, literals and the borrow spelling stay the kind's own.
    registry->builtins.string_type = string_builtin_create(registry, false);

    // Same layout and same name: 'ref' is spelled in the type spec rather than
    // being a type a script can name, so this one is never registered under a
    // name of its own. It differs only in the field naming the characters,
    // which borrows here and owns there.
    registry->builtins.ref_string_type = string_builtin_create(registry, true);
    registry->builtins.ref_string_type->is_ref = true;
    registry->builtins.ref_string_type->owner = registry->builtins.string_type;

    // The VM and the host both read these two fields as GabStringValue, so the
    // computed layout and the C struct are two statements of one thing.
    assert(registry->builtins.string_type->size == sizeof(GabStringValue));
    assert(registry->builtins.string_type->alignment == _Alignof(GabStringValue));

    // Poison type. Deliberately never given a name in any scope: no script can
    // name it, it only arises from a failed resolution.
    registry->builtins.error_type = register_builtin(registry, TYPE_ERROR, "<error>", 0, 1);
}

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->indirects = indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->ref_indirects =
        indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->buffers = indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    registry->strings = strings;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    indirect_map_destroy(registry->indirects);
    indirect_map_destroy(registry->ref_indirects);
    indirect_map_destroy(registry->buffers);
}

Type *type_registry_indirect_to(TypeRegistry *registry, Type *inner) {
    return type_registry_indirect_to_kind(registry, inner, false);
}

Type *type_registry_buffer_of(TypeRegistry *registry, Type *element) {
    Type **existing = indirect_map_lookup(registry->buffers, element);
    if (existing) {
        return *existing;
    }

    // No name, for the reason an indirection has none: it is structural, and a
    // diagnostic derives its printed form from the element.
    Type *type = type_create(registry->arena, TYPE_UNKNOWN, NULL);

    // The element's width, which is the stride a walk over the block advances
    // by. Not the block's own size: how many elements are there is the count in
    // whichever header names it, and no two buffers of one element differ.
    type->size = element->size;
    type->alignment = element->alignment;
    type->inner = element;

    // Never its own dropper, however the element answers. Only the header that
    // names the block knows how many of its elements are live -- the rest is
    // memory nobody has written yet, and dropping it would walk uninitialised
    // slots.
    type->drop = NULL;

    indirect_map_insert(registry->buffers, element, type);

    return type;
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

    object_select_drop(type);

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
    case TYPE_BYTE:
        return registry->builtins.byte_type;
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
    abort();
}
