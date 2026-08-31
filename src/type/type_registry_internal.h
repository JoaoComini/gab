#ifndef GAB_TYPE_REGISTRY_INTERNAL_H
#define GAB_TYPE_REGISTRY_INTERNAL_H

#include "type_internal.h"
#include "type_registry.h"

#define type_intern_hash(key) type_structural_hash(key)
#define type_intern_key_equals(key, other) type_structurally_equals(key, other)
#define type_intern_key_dup(key) key
#define type_intern_entry_free(key, value)

GAB_HASH_MAP(TypeInternTable, type_intern, const Type *, Type *)

/* Keyed on the declaration rather than the type, so every instantiation of an owner finds the one entry. */
typedef struct OwnedKey {
    const TypeDef *owner;

    const String *name;
} OwnedKey;

#define owned_key_hash(key) (((size_t)(key).owner * 31) ^ (size_t)(key).name)
#define owned_key_key_equals(key, other) ((key).owner == (other).owner && (key).name == (other).name)
#define owned_key_key_dup(key) key
#define owned_key_entry_free(key, value)

GAB_HASH_MAP(OwnedTable, owned_key, OwnedKey, Function *)

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

    OwnedTable *owned;

    DropTable *drops;

    DerefTable *derefs;

    LayoutTable *layouts;

    const Type *params[GAB_MAX_TYPE_PARAMS];

    TypePrimitives primitives;
} TypeRegistry;

#endif
