#ifndef GAB_TYPE_REGISTRY_H
#define GAB_TYPE_REGISTRY_H

#include "string/string.h"
#include "type.h"
#include "util/hash_map.h"

#define TYPE_REGISTRY_INITIAL_CAPACITY 8

#define type_map_hash(key) (size_t)key
#define type_map_key_equals(key, other) key == other
#define type_map_key_dup(key) key
#define type_map_entry_free(key, value) type_destroy(value)

GAB_HASH_MAP(TypeMap, type_map, String *, Type *)

typedef struct {
    Type *int_type;
    Type *float_type;
    Type *bool_type;
} TypeBuiltins;

typedef struct {
    TypeMap *map;
    TypeBuiltins builtins;
} TypeRegistry;

TypeRegistry *type_registry_create();
void type_registry_destroy(TypeRegistry *registry);

Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind type);
Type *type_registry_get(TypeRegistry *registry, String *name);

#endif
