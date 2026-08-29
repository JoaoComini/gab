#ifndef GAB_TYPE_REGISTRY_INTERNAL_H
#define GAB_TYPE_REGISTRY_INTERNAL_H

// The registry's own state, for the type module alone.
//
// Everything outside asks through type_registry.h, for the reason struct Type is
// private: what a registry holds is interned, memoized and derived in a
// particular order, and a reader that could reach past that could disagree with
// it.

#include "type_internal.h"
#include "type_registry.h"

// Every constructed type is interned on what it is built of, so that two
// mentions of 'box T' or of '[int; 3]' yield the same Type *: the whole type
// system compares by pointer identity, and a fresh Type per mention would
// silently break every comparison.
//
// One table for every constructor rather than one table each. A type's kind is
// part of what is hashed, so 'box T' and 'ref T' cannot collide, and a generic
// 'List T' or 'Map K,V' needs no table of its own.
//
// Keyed by the type itself, which is also the value: a caller builds one on its
// stack to look up with, and a miss copies it into the registry's arena, since
// an entry has to outlive that stack.
#define type_intern_hash(key) type_structural_hash(key)
#define type_intern_key_equals(key, other) type_structurally_equals(key, other)
#define type_intern_key_dup(key) key
#define type_intern_entry_free(key, value)

GAB_HASH_MAP(TypeInternTable, type_intern, const Type *, Type *)

// What may be called on a type, by the declaration it was declared on and the
// name it answers to.
//
// Keyed by the declaration rather than by a type, so one statement of a method
// serves every instantiation of it: 'Vec<int>' and 'Vec<bool>' reach the same
// entry, and what tells their signatures apart is the arguments each was
// applied to, supplied where the entry is read. A plain struct is a declaration
// applied to no arguments and needs no special case.
//
// Beside the types rather than on them: a method set grows as a program is read
// -- a later statement, a later unit, or the host before any of them -- while
// what a type is was settled when it was interned. Keeping them apart is what
// lets a type be finished when the registry hands it over.
typedef struct MethodKey {
    // The declaration a method was declared on. NULL for a type nothing
    // declares -- a primitive, or one of the built-in constructors -- which
    // owns its methods against the type itself, since it has no declaration to
    // hang them on. That is rustc's shape too: an inherent impl on a primitive
    // is keyed by the type rather than by an AdtDef.
    const TypeDef *def;

    // Set only when there is no declaration, and NULL whenever there is: one of
    // the two names the owner, never both, so a method cannot be reachable
    // under two keys.
    const Type *type;

    const String *name;
} MethodKey;

#define method_key_hash(key) ((((size_t)(key).def * 31) ^ ((size_t)(key).type * 17)) ^ (size_t)(key).name)
#define method_key_key_equals(key, other)                                                                    \
    ((key).def == (other).def && (key).type == (other).type && (key).name == (other).name)
#define method_key_key_dup(key) key
#define method_key_entry_free(key, value)

GAB_HASH_MAP(MethodTable, method_key, MethodKey, Symbol *)

/*
    A signature read under one instantiation's arguments, by the type asked.

    The cache rather than the storage: what a declaration states lives in the
    table above, over its parameters, and substituting it for a given
    instantiation is derived from that. Kept so that two calls on one type get
    the one Symbol the first built rather than an equal second one -- which is
    what lets a caller compare what it was handed by identity.

    Nothing invalidates it: a declaration's methods are stated before any call
    reads one. A later statement adding to a declaration already read would need
    this dropped for that declaration's instantiations.
*/
#define signature_key_hash(key) (((size_t)(key).type * 31) ^ (size_t)(key).name)
#define signature_key_key_equals(key, other) ((key).type == (other).type && (key).name == (other).name)
#define signature_key_key_dup(key) key
#define signature_key_entry_free(key, value)

typedef struct SignatureKey {
    const Type *type;
    const String *name;
} SignatureKey;

GAB_HASH_MAP(SignatureTable, signature_key, SignatureKey, Symbol *)

/*
    What freeing a value of each type does, by the type it was derived from.

    Beside the types rather than on them, for the reason a method set is: what a
    type is was settled when it was interned, while what freeing one does is
    derived from its layout and from what every field of it owns. Keeping them
    apart is what lets a type be finished when the registry hands it over.

    Interned because the derivation recurses: a struct of two fields of one type
    asks that type once, and a ring through a 'box' terminates because the
    second demand finds the first.
*/
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

/*
    Where a value of each type sits in memory, by the type it was derived from.

    Beside the types for the reason a drop plan is, and computed the same way:
    on first demand, from what the type is built of, and memoized so that two
    mentions of a width agree. What a generic instantiation lays out will be
    another entry here rather than another Type field to keep in step.

    Owned by the registry's arena, so a layout outlives every compile that reads
    it and is freed with the types it describes.
*/
#define layout_key_hash(key) (size_t)key
#define layout_key_key_equals(key, other) key == other
#define layout_key_key_dup(key) key
#define layout_key_entry_free(key, value)

GAB_HASH_MAP(LayoutTable, layout_key, const Type *, const TypeLayout *)

/*
    The types the language itself is written in terms of: what a literal
    produces, what an operator answers, what a spec spells without anything
    having been registered.

    Primitives rather than a standard library. A resolver holds these with no VM
    in sight, because nothing about 'let n: int = 1 + 2' asks for a runtime. What
    a VM provides -- 'String', 'Vec<T>', and whatever a host adds beside them --
    is registered instead, and is absent from a compile that never had one.
*/
typedef struct {
    const Type *int_type;

    // One byte, which is what a string's characters are. Not spellable in the
    // language: it exists so that the pointer naming those characters carries a
    // stride, the way every other 'ptr T' does.
    const Type *byte_type;
    const Type *float_type;
    const Type *bool_type;

    // 'str'. The characters of a string, borrowed: where they are and how many
    // there are, owning nothing. A distinct interned Type from the owning one
    // because ownership is read off the type, and a literal and an owned copy
    // must not answer it the same way.
    //
    // Not what 'ref String' names. That is an indirection to a slot holding a
    // header, which is what 'ref' builds for every type in the language.
    const Type *str_type;

    const Type *error_type;
} TypePrimitives;

typedef struct TypeRegistry {
    Arena *arena;

    // Every type built by applying a constructor: 'box T', 'ref T', 'ptr T',
    // '[T; N]', and whatever a generic declaration adds. Keyed by what the type
    // is built of, so the kind is part of what is looked up and two
    // constructors given the same argument never collide.
    TypeInternTable *applications;

    // Every method declared on every declaration, however far apart the
    // statements were.
    MethodTable *methods;

    // What each instantiation reads those as, substituted under its own
    // arguments.
    SignatureTable *signatures;

    // What freeing a value of each type does.
    DropTable *drops;

    // What each type derefs to, for the few that do.
    DerefTable *derefs;

    // How wide a value of each type is, and where its fields begin.
    LayoutTable *layouts;

    // The parameters, interned by index. One set for every declaration: a
    // parameter is a position rather than an identity, so 'Vec's element and
    // whatever a second declaration writes at index zero are one type.
    const Type *params[GAB_MAX_TYPE_PARAMS];

    TypePrimitives primitives;

} TypeRegistry;

#endif
