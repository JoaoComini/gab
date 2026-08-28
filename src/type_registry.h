#ifndef GAB_TYPE_REGISTRY_H
#define GAB_TYPE_REGISTRY_H

#include "arena.h"
#include "string/string.h"
#include "type.h"
#include "util/hash_map.h"

#include <stdbool.h>

#define TYPE_REGISTRY_INITIAL_CAPACITY 8

// Declared rather than included: a method is an ordinary function Symbol, and
// Symbol's own header reaches back here.
typedef struct Symbol Symbol;

// The named types of one scope. Scope owns these and chains them, the same way
// it chains symbol tables.
typedef struct {
    const Type *type;
} TypeBinding;

#define type_map_hash(key) (size_t)key
#define type_map_key_equals(key, other) key == other
#define type_map_key_dup(key) key

GAB_HASH_MAP(TypeMap, type_map, String *, TypeBinding)

// Every constructed type is interned on the application that built it, so that
// two mentions of 'box T' or of '[int; 3]' yield the same Type *: the whole
// type system compares by pointer identity, and a fresh Type per mention would
// silently break every comparison.
//
// One table for every constructor rather than one table each. The key carries
// which constructor and how many arguments, so 'box T' and 'ref T' cannot
// collide, and a generic 'List T' or 'Map K,V' needs no table of its own.
//
// The key's argument array is copied into the registry's arena on insert: a
// caller builds one on its stack to look up with, and an entry has to outlive
// that.
#define type_app_map_hash(key) type_app_hash_of(key)
#define type_app_map_key_equals(key, other) type_app_equals(key, other)
#define type_app_map_key_dup(key) key

GAB_HASH_MAP(TypeAppMap, type_app_map, TypeApp, Type *)

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

    // 'Array', the bare name. Not a usable type on its own -- every array is
    // '[T; N]' for some element -- but the name a spec resolves to before its
    // element is applied, and what a diagnostic prints when one is missing.
    const Type *array_type;

    const Type *error_type;
} TypePrimitives;

/*
    Declaring a type a standard library provides, which a scope then finds by
    name the way it finds a primitive.

    Filled in once and handed over, rather than built up by a provider holding a
    half-made Type: what a type is arrives in one statement, and the registry is
    what interns it, lays it out and settles how it frees. That is also what
    keeps the library out of the language -- a compile with no VM declares
    nothing, so nothing in the resolver names a 'String'.
*/

// One field of a declared struct.
typedef struct TypeFieldDecl {
    const char *name;
    const Type *type;
} TypeFieldDecl;

// What a standard library says a type is.
//
// Either a struct with fields, or a generic declaration whose instantiations
// have them -- 'generic' decides which, and the two are never both given.
typedef struct TypeDecl {
    const char *name;

    // A struct's fields, laid out in the order given.
    const TypeFieldDecl *fields;
    size_t field_count;

    // Or the declaration every instantiation is built from, for a generic.
    const GenericDecl *generic;

    // What a value of this type stands for, and which of its bytes name that
    // view. Both or neither: a deref with no parts could not be lent, and parts
    // with nothing to deref to name nothing. NULL for a type that stands only
    // for itself.
    const Type *derefs_to;
    const LentPart *lent_parts;
    size_t lent_part_count;
} TypeDecl;

// Interning, not naming. One registry per VM holds the primitives and every
// pointer type, because the type system compares types by pointer identity: a
// second 'int' or a second 'box Player' would silently break every comparison.
//
// Which type names are visible where is a scoping question, so the name map
// belongs to Scope, which already owns the parent chain that answers it.
// What may be called on a type, keyed by the type it was declared on and the
// name it answers to.
//
// Beside the types rather than on them: a method set grows as a program is read
// -- a later statement, a later unit, or the host before any of them -- while
// what a type is was settled when it was interned. Keeping them apart is what
// lets a type be finished when the registry hands it over.
typedef struct MethodKey {
    const Type *type;
    const String *name;
} MethodKey;

#define method_key_hash(key) (((size_t)(key).type * 31) ^ (size_t)(key).name)
#define method_key_key_equals(key, other) ((key).type == (other).type && (key).name == (other).name)
#define method_key_key_dup(key) key
#define method_key_entry_free(key, value)

GAB_HASH_MAP(MethodTable, method_key, MethodKey, Symbol *)

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

/*
    What each type derefs to: the borrowed view an owner reaches its methods
    through. 'String' derefs to 'str', which is how a string finds the methods
    written for characters.

    The relation Rust spells Deref, and registered for the same reason its
    impls are: nothing about a type's shape says what it stands for. A 'String'
    and a 'Vec<byte>' are laid out identically, so only the declaration saying
    one denotes text tells them apart.

    Registered rather than derived, and one direction only: an owner reaches its
    view, never the reverse. What belongs to the owner -- duplicating the
    allocation -- must not be reachable from a borrow of it.
*/
#define deref_key_hash(key) (size_t)key
#define deref_key_key_equals(key, other) key == other
#define deref_key_key_dup(key) key
#define deref_key_entry_free(key, value)

/*
    What a type derefs to, and which of its bytes a reference to that view is
    built from.

    One statement, because they are one fact: saying a 'String' stands for a run
    of characters is saying that its block's address and length are what naming
    that run takes. A shape cannot imply either half -- a 'Vec<byte>' is laid
    out identically and stands for nothing.
*/
typedef struct Deref {
    const Type *to;

    LentPart parts[GAB_MAX_LENT_PARTS];
    size_t part_count;
} Deref;

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
    What turns one of a declaration's methods into a Symbol an instantiation
    answers to.

    A hook because the extern table a C body is numbered in belongs to the VM,
    while interning happens wherever a type is named -- including in a resolve
    with no VM at all. So the VM installs this when it builds, and an
    instantiation made without one simply declares no methods.

    'signature' holds the receiver, then what the caller writes; 'count' is how
    many that is in total.
*/
typedef void (*MethodInstaller)(void *ctx, const Type *on, const char *name, void *body,
                                const Type *return_type, const Type *const *signature, size_t count);

typedef struct TypeRegistry {
    Arena *arena;

    // How an instantiation's methods become Symbols, and what to call it with.
    // NULL where nothing registered one, which is every compile that runs
    // without a VM.
    MethodInstaller install_method;
    void *install_ctx;

    StringPool *strings;

    // Every type built by applying a constructor: 'box T', 'ref T', 'ptr T',
    // '[T; N]', and whatever a generic declaration adds. Keyed by the
    // application, so the constructor is part of what is looked up and two
    // constructors given the same argument never collide.
    TypeAppMap *applications;

    // Every method declared on every type, however far apart the declarations
    // were.
    MethodTable *methods;

    // What freeing a value of each type does.
    DropTable *drops;

    // What each type derefs to, for the few that do.
    DerefTable *derefs;

    // How wide a value of each type is, and where its fields begin.
    LayoutTable *layouts;

    TypePrimitives primitives;

} TypeRegistry;

// Declares a method, or fails if the type already answers that name. Finds one
// by following 'owner' when the type itself does not answer, so that a type
// sharing another's identity reaches its set.
bool type_registry_add_method(TypeRegistry *registry, const Type *type, String *name, Symbol *method);
Symbol *type_registry_find_method(TypeRegistry *registry, const Type *type, const String *name);

// Declares a nominal type, and hands back the Type nothing else may build.
//
// The type comes back with no layout, because a declaration does not have one:
// its name is bound here so that a field may name it, and its width follows
// later from fields that may name types not yet declared. What may be done with
// such a type is what needs no width -- name it, point at it -- which is what
// makes 'struct A { b: box B }' resolve before B exists.
Type *type_registry_declare_struct(TypeRegistry *registry, String *name, size_t max_fields);

/*
    What freeing a value of this type does, or NULL when it owns nothing.

    Derived on first demand and memoized. Every offset it needs is read off the
    type, which is laid out by the time anything can ask: a type is interned
    with its layout, and a struct is laid out where it is declared.
*/
const DropPlan *type_registry_drop_of(TypeRegistry *registry, const Type *type);

/*
    How wide a value of this type is, what it aligns to, and where its fields
    begin. Never NULL: a type with no width of its own answers zero, which is
    what an unsized type and a bare declaration both are.

    Derived on first demand and memoized. A struct's layout follows from its
    fields, so asking for one before every field type is interned would settle a
    width from types not yet laid out -- which is why the resolver asks for it
    where the struct is completed rather than where it is declared.
*/
const TypeLayout *type_registry_layout_of(TypeRegistry *registry, const Type *type);

// The two questions a layout is most often asked, for callers that want a
// number rather than the layout it came from.
size_t type_registry_size_of(TypeRegistry *registry, const Type *type);
size_t type_registry_align_of(TypeRegistry *registry, const Type *type);

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings);

void type_registry_destroy(TypeRegistry *registry);

const Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind type);

// Interns what 'decl' describes, lays it out and settles how it frees. Returns
// the finished type, which is the provider's handle to it.
//
// Interning only. What names a type is a Scope, which is where every other name
// lives -- so a provider declares the type here and binds it there.
const Type *type_registry_declare(TypeRegistry *registry, const TypeDecl *decl);
// Gives a type the borrowed view it derefs to, through which it answers the
// methods written for that view. One direction: 'from' reaches 'to', never the
// reverse.
void type_registry_set_deref(TypeRegistry *registry, const Type *from, const Type *to, const LentPart *parts,
                             size_t part_count);

// What a type derefs to, or NULL for one that derefs to nothing. What a method
// lookup walks, and what tells an owner apart from a value merely laid out like
// it.
const Type *type_registry_deref_of(TypeRegistry *registry, const Type *type);

// The deref relation itself, parts included, or NULL for a type that derefs to
// nothing. What a lend reads to know which bytes to copy.
const Deref *type_registry_deref(TypeRegistry *registry, const Type *type);

const Type *type_registry_error_type(TypeRegistry *registry);

// The interned '[element; N]': a header of {data, length} owning a block of
// elements. One per element type, and the type that carries the element -- the
// raw address naming the block does not, so this is what supplies the walk that
// frees them.
const Type *type_registry_array_of(TypeRegistry *registry, const Type *element, int32_t length);

// The interned 'box inner' and 'ref inner'. Two constructors rather than one
// taking a flag: which of them a type is decides whether a slot holding it
// frees what it names, and that is the question the kind exists to answer.
const Type *type_registry_box_to(TypeRegistry *registry, const Type *inner);
const Type *type_registry_ref_to(TypeRegistry *registry, const Type *inner);

// The interned 'ptr pointee': an address the size of a machine pointer, owning
// nothing. Distinct from both indirections of the same pointee, since what a
// slot must free is read off the type.
const Type *type_registry_ptr_to(TypeRegistry *registry, const Type *pointee);

/*
    The interned instantiation of a generic declaration: 'Vec<int>' from 'Vec'.

    One path for every generic type rather than one per name. What differs
    between two declarations is the fields they declare and how many parameters
    they take, and both of those are read off the declaration -- so a second
    generic name needs an entry where the builtins are built and nothing here.

    The arguments are interned into the application key, so two mentions of
    'Vec<int>' find one Type.
*/
const Type *type_registry_instantiate(TypeRegistry *registry, const Type *decl, const Type *const *args,
                                      size_t arg_count);

// The interned 'block element': an owning address and the capacity it was
// allocated at. What a collection holds its elements in, and the one owning
// shape whose extent is a number rather than its pointee's type.
const Type *type_registry_block_of(TypeRegistry *registry, const Type *element);

#endif
