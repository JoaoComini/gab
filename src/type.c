#include "type.h"
#include <stdlib.h>

Type *type_create(Arena *arena, TypeKind kind, String *name) {
    Type *type = arena_alloc(arena, sizeof(Type));
    type->kind = kind;
    type->name = name;

    return type;
}

void type_destroy(Type *type) { free(type); }

TypeSpec *type_spec_create(StringRef name) {
    TypeSpec *spec = malloc(sizeof(TypeSpec));
    spec->name = name;

    return spec;
}

void type_spec_destroy(TypeSpec *spec) { free(spec); }
