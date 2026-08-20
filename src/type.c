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
    type->pointee = NULL;
    type->methods = NULL;
    type->is_ref = false;

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

bool type_is_pointer(const Type *type) { return type && type->kind == TYPE_POINTER; }

// Sized for a handful: most struct types declare no methods at all, and the map
// grows if one proves popular.
#define METHOD_MAP_INITIAL_CAPACITY 4

bool type_add_method(Arena *arena, Type *type, String *name, Symbol *method) {
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
    if (!type || !type->methods) {
        return NULL;
    }

    Symbol **found = method_map_lookup(type->methods, (String *)name);

    return found ? *found : NULL;
}

TypeSpec *type_spec_create(StringRef name, unsigned int pointer_depth, bool is_ref) {
    TypeSpec *spec = malloc(sizeof(TypeSpec));
    spec->name = name;
    spec->pointer_depth = pointer_depth;
    spec->is_ref = is_ref;

    return spec;
}

void type_spec_destroy(TypeSpec *spec) { free(spec); }
