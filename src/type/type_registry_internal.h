#ifndef GAB_TYPE_REGISTRY_INTERNAL_H
#define GAB_TYPE_REGISTRY_INTERNAL_H

#include "decl.h"
#include "type_internal.h"
#include "type_registry.h"

#define type_intern_hash(key) type_structural_hash(key)
#define type_intern_key_equals(key, other) type_structurally_equals(key, other)

GAB_HASH_MAP(TypeInternTable, type_intern, const Type *, Type *)

#define conformance_key_hash(key) conformance_key_hash_of(key)
#define conformance_key_key_equals(key, other) conformance_key_equals(key, other)

GAB_HASH_MAP(ConformanceTable, conformance_key, ConformanceKey, bool)

#define layout_key_hash(key) (size_t)key
#define layout_key_key_equals(key, other) key == other

GAB_HASH_MAP(LayoutTable, layout_key, const Type *, const TypeLayout *)

typedef struct {
    const Type *i32_type;

    const Type *u8_type;
    const Type *usize_type;
    const Type *f32_type;
    const Type *bool_type;

    const Type *str_type;

    /* Every slice shares one declaration, so a method reaches 'slice<T>' at any element. */
    const TypeDecl *slice_decl;

    /* Every array shares one declaration, which is what carries the conformance all of them have. */
    const TypeDecl *array_decl;

    /* Every raw run shares one declaration, which is what carries the indexing all of them have. */
    const TypeDecl *raw_decl;

    const Type *error_type;
} TypePrimitives;

typedef struct TypeRegistry {
    Arena *arena;

    TypeInternTable *applications;

    ConformanceTable *conformances;

    LayoutTable *layouts;

    const Type *params[GAB_MAX_TYPE_PARAMS];

    TypePrimitives primitives;

    KnownNames names;

    IntrinsicLowering intrinsics[GAB_INTRINSIC_COUNT];
} TypeRegistry;

#endif
