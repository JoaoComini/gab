#include "type.h"

#include "util/align.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

_Static_assert(sizeof(int32_t) == 4, "gab int must be 4 bytes");
_Static_assert(sizeof(float) == 4, "gab float must be 4 bytes");

Type *type_create(Arena *arena, TypeKind kind, String *name) {
    Type *type = arena_alloc(arena, sizeof(Type));
    type->kind = kind;
    type->name = name;
    type->size = 0;
    type->alignment = 1;
    type->fields = NULL;
    type->field_count = 0;
    type->inner = NULL;
    type->methods = NULL;
    type->is_ref = false;
    type->owner = NULL;

    return type;
}

Type *type_struct_create(Arena *arena, String *name, size_t max_fields) {
    Type *type = type_create(arena, TYPE_STRUCT, name);

    if (max_fields > 0) {
        type->fields = arena_alloc(arena, max_fields * sizeof(TypeField));
    }

    return type;
}

void type_add_field(Type *type, String *name, Type *field_type) {
    assert(type->fields && "struct was created without room for fields");

    type->fields[type->field_count++] = (TypeField){
        .name = name,
        .type = field_type,
        .offset = 0,
    };
}

void type_layout_compute(Type *type) {
    size_t offset = 0;
    size_t alignment = 1;

    for (size_t i = 0; i < type->field_count; i++) {
        TypeField *field = &type->fields[i];

        offset = align_up(offset, field->type->alignment);
        field->offset = offset;
        offset += field->type->size;

        if (field->type->alignment > alignment) {
            alignment = field->type->alignment;
        }
    }

    type->alignment = alignment;
    type->size = align_up(offset, alignment);
}

const TypeField *type_find_field(const Type *type, const String *name) {
    for (size_t i = 0; i < type->field_count; i++) {
        const TypeField *field = &type->fields[i];

        if (field->name == name) {
            return field;
        }
    }

    return NULL;
}

bool type_field_offset(const Type *type, const String *name, size_t *out_offset) {
    const TypeField *field = type_find_field(type, name);

    if (!field) {
        return false;
    }

    *out_offset = field->offset;
    return true;
}

bool type_is_indirect(const Type *type) { return type && type->kind == TYPE_INDIRECT; }

bool type_is_owned(const Type *type) {
    if (!type) {
        return false;
    }

    // A 'ref' names memory without owning it, whatever kind it qualifies.
    if (type->is_ref) {
        return false;
    }

    if (type->kind == TYPE_INDIRECT || type->kind == TYPE_STRING) {
        return true;
    }

    // A struct is not itself an owner: it owns through whichever fields do, and
    // is freed field by field rather than as one value.
    for (size_t i = 0; i < type->field_count; i++) {
        if (type_is_owned(type->fields[i].type)) {
            return true;
        }
    }

    return false;
}

// Whether a value of this type can be duplicated by copying its bytes. An
// owning value cannot: two slots holding it would both free it. Anything
// reaching one transitively inherits that, so a struct is copyable exactly
// when every field is.
//
// Derived from the type rather than declared on it, so a struct becomes
// non-copyable the moment it is given a field that owns, and no declaration
// can disagree with what the type holds.
bool type_is_copyable(const Type *type) {
    if (!type) {
        return true;
    }

    // Exactly the values that own: copying one would make a second owner of
    // memory only one of them may free.
    if (type->kind == TYPE_INDIRECT || type->kind == TYPE_STRING) {
        return type->is_ref;
    }

    for (size_t i = 0; i < type->field_count; i++) {
        if (!type_is_copyable(type->fields[i].type)) {
            return false;
        }
    }

    return true;
}

// Sized for a handful: most struct types declare no methods at all, and the map
// grows if one proves popular.
#define METHOD_MAP_INITIAL_CAPACITY 4

bool type_add_method(Arena *arena, Type *type, String *name, Symbol *method) {
    // Declared on the owning type, which is where lookup reads: a borrow shares
    // the method set of what it borrows rather than keeping one of its own.
    if (type->owner) {
        type = type->owner;
    }

    if (!type->methods) {
        type->methods = method_map_create_alloc(arena_allocator(arena), METHOD_MAP_INITIAL_CAPACITY);
    }

    if (method_map_lookup(type->methods, name)) {
        return false;
    }

    method_map_insert(type->methods, name, method);
    return true;
}

Symbol *type_find_method(const Type *type, const String *name) {
    if (!type) {
        return NULL;
    }

    // A borrow shares the method set of what it borrows: 'ref string' and
    // 'string' are one type in two ownership states, and a method that only
    // reads its receiver is meaningful on both. Declaring lands on the owning
    // type, which is where 'owner' points.
    if (type->owner) {
        type = type->owner;
    }

    if (!type->methods) {
        return NULL;
    }

    Symbol **found = method_map_lookup(type->methods, (String *)name);

    return found ? *found : NULL;
}

TypeSpec *type_spec_create(StringRef name, unsigned int indirect_depth, uint32_t ref_levels) {
    TypeSpec *spec = malloc(sizeof(TypeSpec));
    spec->name = name;
    spec->indirect_depth = indirect_depth;
    spec->ref_levels = ref_levels;

    return spec;
}

void type_spec_destroy(TypeSpec *spec) { free(spec); }
