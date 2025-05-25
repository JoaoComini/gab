#ifndef GAB_TYPE_REGISTRY_H
#define GAB_TYPE_REGISTRY_H

#include "hash_map.h"
#include "type.h"

#define TYPE_REGISTRY_INITIAL_CAPACITY 8

GAB_HASH_MAP(TypeRegistry, type_registry, Type)

void type_registry_register_builtin(TypeRegistry *registry);
Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind type);

#endif
