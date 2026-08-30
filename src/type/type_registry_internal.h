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
    const TypeDef *def;

    const Type *type;

    const String *name;
} MethodKey;

#define method_key_hash(key) ((((size_t)(key).def * 31) ^ ((size_t)(key).type * 17)) ^ (size_t)(key).name)
#define method_key_key_equals(key, other)                                                                    \
    ((key).def == (other).def && (key).type == (other).type && (key).name == (other).name)
#define method_key_key_dup(key) key
#define method_key_entry_free(key, value)

GAB_HASH_MAP(MethodTable, method_key, MethodKey, Symbol *)

#define generic_key_hash(key) method_key_hash(key)
#define generic_key_key_equals(key, other) method_key_key_equals(key, other)
#define generic_key_key_dup(key) key
#define generic_key_entry_free(key, value)

GAB_HASH_MAP(GenericTable, generic_key, MethodKey, GenericMethod *)

#define signature_key_hash(key) (((size_t)(key).type * 31) ^ (size_t)(key).name)
#define signature_key_key_equals(key, other) ((key).type == (other).type && (key).name == (other).name)
#define signature_key_key_dup(key) key
#define signature_key_entry_free(key, value)

typedef struct SignatureKey {
    const Type *type;
    const String *name;
} SignatureKey;

GAB_HASH_MAP(SignatureTable, signature_key, SignatureKey, Symbol *)

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

    GenericTable *generics;

    SignatureTable *signatures;

    DropTable *drops;

    DerefTable *derefs;

    LayoutTable *layouts;

    const Type *params[GAB_MAX_TYPE_PARAMS];

    TypePrimitives primitives;
} TypeRegistry;

#endif
