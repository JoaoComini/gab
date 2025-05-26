#ifndef GAB_TYPE_REGISTRY_H
#define GAB_TYPE_REGISTRY_H

#include "string/string.h"
#include "type.h"
#include "util/hash_map.h"

#define TYPE_REGISTRY_INITIAL_CAPACITY 8

#define type_registry_hash(key) (size_t)key
#define type_registry_key_equals(key, other) key == other
#define type_registry_key_dup(key) key
#define type_registry_entry_free(key, value)

GAB_HASH_MAP(TypeRegistry, type_registry, String *, Type)

void type_registry_register_builtin(TypeRegistry *registry);
Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind type);

#endif
