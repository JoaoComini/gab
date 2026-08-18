#ifndef GAB_TYPE_REGISTRY_H
#define GAB_TYPE_REGISTRY_H

#include "arena.h"
#include "string/string.h"
#include "type.h"
#include "util/hash_map.h"

#include <stdbool.h>

#define TYPE_REGISTRY_INITIAL_CAPACITY 8

#define type_map_hash(key) (size_t)key
#define type_map_key_equals(key, other) key == other
#define type_map_key_dup(key) key

GAB_HASH_MAP(TypeMap, type_map, String *, Type *)

// Pointer types are interned on their pointee so that every mention of *T
// yields the same Type *: the whole type system compares by pointer identity,
// and a fresh Type per mention would silently break every comparison.
#define pointer_map_hash(key) (size_t)key
#define pointer_map_key_equals(key, other) key == other
#define pointer_map_key_dup(key) key

GAB_HASH_MAP(PointerMap, pointer_map, Type *, Type *)

typedef struct {
    Type *int_type;
    Type *float_type;
    Type *bool_type;
    Type *error_type;
} TypeBuiltins;

typedef struct {
    Arena *arena;

    StringPool *strings;

    TypeMap *map;
    PointerMap *pointers;
    TypeBuiltins builtins;
} TypeRegistry;

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings);
void type_registry_destroy(TypeRegistry *registry);

Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind type);
Type *type_registry_get(TypeRegistry *registry, String *name);
bool type_registry_register(TypeRegistry *registry, Type *type);
Type *type_registry_error_type(TypeRegistry *registry);

// The interned *pointee. Repeated calls with the same pointee return the same
// Type *.
Type *type_registry_pointer_to(TypeRegistry *registry, Type *pointee);

#endif
