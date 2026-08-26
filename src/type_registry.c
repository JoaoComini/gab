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

// The 'String' type, laid out from its two fields. 'is_ref' decides only
// whether the field naming the characters owns them, which is the whole
// difference between a string and a view of one -- and is where every later
// question about ownership reads its answer, rather than from a bit beside it.
static Type *string_builtin_create(TypeRegistry *registry, bool is_ref) {
    // Each half carries its own name: 'String' owns its characters, 'str'
    // borrows someone else's. Two names rather than one, because the two differ
    // in what a slot holding them must free, and a diagnostic naming both
    // 'String' would read as a mismatch between a type and itself.
    const char *name = is_ref ? "str" : "String";
    Type *type = type_struct_create(registry->arena, string_from_cstr(registry->strings, name), 2);
    type->kind = TYPE_STRING;

    Type *characters = type_registry_indirect_to_kind(registry, registry->builtins.buffer_type, is_ref);

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

    // Sized as one byte: a buffer holds no element type, so what a walk over it
    // advances by comes from the header, and this stride is the unit that
    // 'object_alloc_sized' counts its bytes in.
    registry->builtins.buffer_type = register_builtin(registry, TYPE_BUFFER, "buffer", 1, 1);

    // Never a dropper, however its elements answer: only the header naming a
    // block knows how many of its elements are live, so only that header's own
    // drop may walk them. See Type::element.
    registry->builtins.buffer_type->drop = NULL;

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
    registry->arrays = indirect_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    registry->strings = strings;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    indirect_map_destroy(registry->indirects);
    indirect_map_destroy(registry->ref_indirects);
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
                   type_registry_indirect_to_kind(registry, registry->builtins.buffer_type, false));
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
    case TYPE_BUFFER:
        return registry->builtins.buffer_type;
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
    abort();
}
