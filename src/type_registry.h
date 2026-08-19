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
//
// The generation is the compile that declared the name here. A Type is shared
// and interned, so it cannot carry one; the binding of a name to a type is
// what belongs to a compile, and that is this entry.
typedef struct {
    Type *type;
    unsigned int generation;
} TypeBinding;

#define type_map_hash(key) (size_t)key
#define type_map_key_equals(key, other) key == other
#define type_map_key_dup(key) key

GAB_HASH_MAP(TypeMap, type_map, String *, TypeBinding)

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

// Interning, not naming. One registry per VM holds the builtins and every
// pointer type, because the type system compares types by pointer identity: a
// second 'int' or a second '*Player' would silently break every comparison.
//
// Which type names are visible where is a scoping question, so the name map
// belongs to Scope, which already owns the parent chain that answers it.
typedef struct TypeRegistry {
    Arena *arena;

    StringPool *strings;

    PointerMap *pointers;

    // Weak pointer types, interned apart from the strong ones so that
    // pointer-identity comparison keeps telling them apart.
    PointerMap *weak_pointers;
    TypeBuiltins builtins;
} TypeRegistry;

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings);

void type_registry_destroy(TypeRegistry *registry);

Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind type);
Type *type_registry_error_type(TypeRegistry *registry);

// The interned *pointee. Repeated calls with the same pointee return the same
// Type *.
Type *type_registry_pointer_to(TypeRegistry *registry, Type *pointee);

// As type_registry_pointer_to, choosing between the strong and weak flavours.
// A weak pointer does not keep its pointee alive; the two are distinct types.
Type *type_registry_pointer_to_kind(TypeRegistry *registry, Type *pointee, bool weak);

#endif
