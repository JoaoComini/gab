#ifndef GAB_TYPE_REGISTRY_INTERNAL_H
#define GAB_TYPE_REGISTRY_INTERNAL_H

#include "type_internal.h"
#include "type_registry.h"

#define type_intern_hash(key) type_structural_hash(key)
#define type_intern_key_equals(key, other) type_structurally_equals(key, other)
#define type_intern_key_dup(key) key
#define type_intern_entry_free(key, value)

GAB_HASH_MAP(TypeInternTable, type_intern, const Type *, Type *)

typedef struct MethodKey {
    const Type *type;

    const String *name;
} MethodKey;

#define method_key_hash(key) (((size_t)(key).type * 31) ^ (size_t)(key).name)
#define method_key_key_equals(key, other) ((key).type == (other).type && (key).name == (other).name)
#define method_key_key_dup(key) key
#define method_key_entry_free(key, value)

#define method_decl_key_hash(key) method_key_hash(key)
#define method_decl_key_key_equals(key, other) method_key_key_equals(key, other)
#define method_decl_key_key_dup(key) key
#define method_decl_key_entry_free(key, value)

GAB_HASH_MAP(MethodTable, method_decl_key, MethodKey, MethodDecl *)

#define instance_key_hash(key) (((size_t)(key).type * 31) ^ (size_t)(key).name)
#define instance_key_key_equals(key, other) ((key).type == (other).type && (key).name == (other).name)
#define instance_key_key_dup(key) key
#define instance_key_entry_free(key, value)

typedef struct InstanceKey {
    const Type *type;
    const String *name;
} InstanceKey;

GAB_HASH_MAP(InstanceTable, instance_key, InstanceKey, Function *)

#define drop_key_hash(key) (size_t)key
#define drop_key_key_equals(key, other) key == other
#define drop_key_key_dup(key) key
#define drop_key_entry_free(key, value)

GAB_HASH_MAP(DropTable, drop_key, const Type *, const DropPlan *)

#define deref_key_hash(key) (size_t)key
#define deref_key_key_equals(key, other) key == other
#define deref_key_key_dup(key) key
#define deref_key_entry_free(key, value)

GAB_HASH_MAP(DerefTable, deref_key, const Type *, const Deref *)

#define layout_key_hash(key) (size_t)key
#define layout_key_key_equals(key, other) key == other
#define layout_key_key_dup(key) key
#define layout_key_entry_free(key, value)

GAB_HASH_MAP(LayoutTable, layout_key, const Type *, const TypeLayout *)

typedef struct {
    const Type *int_type;

    const Type *byte_type;
    const Type *float_type;
    const Type *bool_type;

    const Type *str_type;

    const Type *error_type;
} TypePrimitives;

typedef struct TypeRegistry {
    Arena *arena;

    TypeInternTable *applications;

    MethodTable *methods;

    InstanceTable *instances;

    DropTable *drops;

    DerefTable *derefs;

    LayoutTable *layouts;

    const Type *params[GAB_MAX_TYPE_PARAMS];

    TypePrimitives primitives;
} TypeRegistry;

#endif
