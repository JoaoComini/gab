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

// The 'String' type, laid out from its two fields. 'is_ref' is the whole
// difference between a string and a view of one: it says whether the header
// owns the characters it names, which the raw address naming them cannot.
static Type *string_builtin_create(TypeRegistry *registry, bool is_ref) {
    // Each half carries its own name: 'String' owns its characters, 'str'
    // borrows someone else's. Two names rather than one, because the two differ
    // in what a slot holding them must free, and a diagnostic naming both
    // 'String' would read as a mismatch between a type and itself.
    const char *name = is_ref ? "str" : "String";
    Type *type = type_struct_create(registry->arena, string_from_cstr(registry->strings, name), 2);
    type->kind = TYPE_STRING;

    // What the two spellings differ in. Read by type_is_owned, since the field
    // naming the characters is a raw address and answers nothing about them.
    type->is_ref = is_ref;

    // A raw address either way: who frees the characters is the header's own
    // business, recorded in 'is_ref' above rather than in this field's type.
    Type *characters = type_registry_ptr_to(registry, registry->builtins.byte_type);

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

    // Sized as one byte, which is the stride a walk over characters advances by
    // and the unit a block of them is counted in.
    registry->builtins.byte_type = register_builtin(registry, TYPE_INT, "byte", 1, 1);

    // A struct in its layout and a builtin in its semantics: the fields are
    // where its size, its alignment and what it owns all come from, while
    // comparison, literals and the borrow spelling stay the kind's own.
    registry->builtins.string_type = string_builtin_create(registry, false);

    // Same layout, differing only in the field naming the characters, which
    // borrows here and owns there. 'ref String' is a different type again: the
    // address of a slot holding a header, which is what 'ref' means everywhere.
    registry->builtins.str_type = string_builtin_create(registry, true);

    registry->builtins.str_type->owner = registry->builtins.string_type;

    // The VM and the host both read these two fields as GabStringValue, so the
    // computed layout and the C struct are two statements of one thing.
    assert(registry->builtins.string_type->size == sizeof(GabStringValue));
    assert(registry->builtins.string_type->alignment == _Alignof(GabStringValue));

    // The bare name every 'Array T' is interned under. Sized as the header the
    // elements make it, so that a diagnostic naming it says something true even
    // though no slot ever holds this type itself.
    registry->builtins.array_type = register_builtin(registry, TYPE_ARRAY, "Array", 0, 1);

    // Poison type. Deliberately never given a name in any scope: no script can
    // name it, it only arises from a failed resolution.
    registry->builtins.error_type = register_builtin(registry, TYPE_ERROR, "<error>", 0, 1);
}

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->indirects = indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->ref_indirects =
        indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->ptrs = indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arrays = indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    registry->strings = strings;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    indirect_map_destroy(registry->indirects);
    indirect_map_destroy(registry->ref_indirects);
    indirect_map_destroy(registry->ptrs);
    indirect_map_destroy(registry->arrays);
}

Type *type_registry_array_of(TypeRegistry *registry, Type *element) {
    Type **existing = indirect_map_lookup(registry->arrays, element);
    if (existing) {
        return *existing;
    }

    // Laid out from its fields exactly as a string is, and for the same reason:
    // the size, the alignment and what it owns all follow from them.
    Type *type = type_struct_create(registry->arena, registry->builtins.array_type->name, 2);
    type->kind = TYPE_ARRAY;
    type->element = element;

    type_add_field(type, string_from_cstr(registry->strings, "data"),
                   type_registry_ptr_to(registry, element));
    type_add_field(type, string_from_cstr(registry->strings, "length"), registry->builtins.int_type);
    type_layout_compute(type);
    object_select_drop(type);

    // Every array answers the same methods, which do not depend on the element:
    // 'len' reads a count. Reached through 'owner' rather than copied into each
    // set, the way a 'str' reaches a 'String's.
    type->owner = registry->builtins.array_type;

    // Interned before the layout is read by anything else, so a recursive
    // element -- an 'Array' of a struct holding one -- finds this entry rather
    // than building a second.
    indirect_map_insert(registry->arrays, element, type);

    return type;
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

    object_select_drop(type);

    indirect_map_insert(map, inner, type);

    return type;
}

Type *type_registry_ptr_to(TypeRegistry *registry, Type *pointee) {
    Type **existing = indirect_map_lookup(registry->ptrs, pointee);
    if (existing) {
        return *existing;
    }

    // Structural like an indirection, so no name of its own: a diagnostic
    // derives one from the pointee. See Type::name.
    Type *type = type_create(registry->arena, TYPE_PTR, NULL);

    type->size = sizeof(void *);
    type->alignment = _Alignof(void *);
    type->inner = pointee;

    // Interned before the drop is selected, so a pointee reaching back here --
    // a struct holding a pointer to its own type -- finds this entry rather
    // than building a second.
    indirect_map_insert(registry->ptrs, pointee, type);

    object_select_drop(type);

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
