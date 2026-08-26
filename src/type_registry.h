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

    // One byte, which is what a string's characters are. Not spellable in the
    // language: it exists so that the pointer naming those characters carries a
    // stride, the way every other 'ptr T' does.
    Type *byte_type;
    Type *float_type;
    Type *bool_type;
    Type *string_type;

    // 'str'. The characters of a string, borrowed: the same two slots as a
    // 'String' and copied like one, owning nothing. A distinct interned Type
    // from the owning one because ownership is read off the type, and a literal
    // and a concatenation must not answer it the same way.
    //
    // Not what 'ref String' names. That is an indirection to a slot holding a
    // header, which is what 'ref' builds for every type in the language.
    Type *str_type;

    // 'Array', the bare name. Not a usable type on its own -- every array is
    // 'Array T' for some element -- but the name a spec resolves to before its
    // element is applied, and what a diagnostic prints when one is missing.
    Type *array_type;

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

    // Raw 'ptr T' types, interned on the pointee. A third map rather than a
    // flag on the other two, because a raw pointer is neither owning nor
    // borrowing and must not compare equal to either.
    IndirectMap *ptrs;

    // 'Array T' types, interned on the element for the same reason: the type
    // system compares by pointer identity, so two mentions of 'Array int' must
    // be one Type.
    IndirectMap *arrays;

    TypeBuiltins builtins;
} TypeRegistry;

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings);

void type_registry_destroy(TypeRegistry *registry);

Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind type);
Type *type_registry_error_type(TypeRegistry *registry);

// The interned *inner. Repeated calls with the same inner return the same
// Type *.
Type *type_registry_indirect_to(TypeRegistry *registry, Type *inner);

// The interned 'Array element': a header of {data, length} owning a block of
// elements. One per element type, and the type that carries the element -- the
// raw address naming the block does not, so this is what supplies the walk that
// frees them.
Type *type_registry_array_of(TypeRegistry *registry, Type *element);

// As type_registry_indirect_to, choosing between the owning and borrowing
// flavours. A 'ref T' does not own its inner; the two are distinct types.
Type *type_registry_indirect_to_kind(TypeRegistry *registry, Type *inner, bool is_ref);

// The interned 'ptr pointee': an address the size of a machine pointer, owning
// nothing. Distinct from both indirections of the same pointee, since what a
// slot must free is read off the type.
Type *type_registry_ptr_to(TypeRegistry *registry, Type *pointee);

#endif
