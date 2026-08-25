#ifndef GAB_TYPE_REGISTRY_H
#define GAB_TYPE_REGISTRY_H

#include "arena.h"
#include "string/string.h"
#include "type.h"
#include "util/hash_map.h"

#include <stdbool.h>

#define TYPE_REGISTRY_INITIAL_CAPACITY 8

// The named types of one scope. Scope owns these and chains them, the same way
// it chains symbol tables.
typedef struct {
    Type *type;
} TypeBinding;

#define type_map_hash(key) (size_t)key
#define type_map_key_equals(key, other) key == other
#define type_map_key_dup(key) key

GAB_HASH_MAP(TypeMap, type_map, String *, TypeBinding)

// An indirection is interned on what it names, so that every mention of
// 'box T' yields the same Type *: the whole type system compares by pointer
// identity, and a fresh Type per mention would silently break every comparison.
#define indirect_map_hash(key) (size_t)key
#define indirect_map_key_equals(key, other) key == other
#define indirect_map_key_dup(key) key

GAB_HASH_MAP(IndirectMap, indirect_map, Type *, Type *)

typedef struct {
    Type *int_type;
    Type *float_type;
    Type *bool_type;
    Type *string_type;

    // 'ref string'. A distinct interned Type from the owning one, for the
    // reason every 'ref T' is: ownership is read off the type, and a literal
    // and a concatenation must not answer it the same way.
    Type *ref_string_type;

    // The header a string's characters carry. Never nameable: it exists so that
    // freeing them frees the bytes and stops, rather than reading them back as
    // a string header naming further characters.
    Type *characters_type;

    Type *error_type;
} TypeBuiltins;

// Interning, not naming. One registry per VM holds the builtins and every
// pointer type, because the type system compares types by pointer identity: a
// second 'int' or a second 'box Player' would silently break every comparison.
//
// Which type names are visible where is a scoping question, so the name map
// belongs to Scope, which already owns the parent chain that answers it.
typedef struct TypeRegistry {
    Arena *arena;

    StringPool *strings;

    IndirectMap *indirects;

    // Borrowing 'ref T' types, interned apart from the owning ones so that
    // pointer-identity comparison keeps telling them apart.
    IndirectMap *ref_indirects;
    TypeBuiltins builtins;
} TypeRegistry;

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings);

void type_registry_destroy(TypeRegistry *registry);

Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind type);
Type *type_registry_error_type(TypeRegistry *registry);

// The interned *inner. Repeated calls with the same inner return the same
// Type *.
Type *type_registry_indirect_to(TypeRegistry *registry, Type *inner);

// As type_registry_indirect_to, choosing between the owning and borrowing
// flavours. A 'ref T' does not own its inner; the two are distinct types.
Type *type_registry_indirect_to_kind(TypeRegistry *registry, Type *inner, bool is_ref);

#endif
