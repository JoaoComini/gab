#ifndef GAB_TYPE_REGISTRY_H
#define GAB_TYPE_REGISTRY_H

#include "arena.h"
#include "string/string.h"
#include "type.h"
#include "type_app.h"
#include "type_layout.h"
#include "util/hash_map.h"

#include <stdbool.h>

#define TYPE_REGISTRY_INITIAL_CAPACITY 8

typedef struct TypeRegistry TypeRegistry;

// What the built-in types are called. Supplied at creation because the registry
// interns no name of its own.
typedef struct TypePrimitiveNames {
    String *int_name;
    String *float_name;
    String *bool_name;
    String *byte_name;
    String *str_name;
    String *array_name;
    String *error_name;
} TypePrimitiveNames;

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
    String *name;
    const Type *type;
} TypeFieldDecl;

// What a standard library says a type is.
//
// Either a struct with fields, or a generic declaration whose instantiations
// have them -- 'generic' decides which, and the two are never both given.
typedef struct TypeDecl {
    // Interned by the provider, as every name the registry is given is: what
    // interns them is the pool the provider already holds, so the registry needs
    // none of its own.
    String *name;

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

/*
    What turns one of a declaration's methods into a Symbol an instantiation
    answers to.

    A hook because the extern table a C body is numbered in belongs to the VM,
    while interning happens wherever a type is named -- including in a resolve
    with no VM at all. So the VM installs this when it builds, and an
    instantiation made without one simply declares no methods.

    Nor can it be deferred to the end of a resolve: 'xs.push(7)' is checked in
    the same pass that interned 'Vec<int>', so what a type answers to must be
    declared the moment it is interned rather than once the unit is done.
*/

// One instantiated method: what a declaration declares, with its parameters
// filled in. A descriptor rather than an argument list, so that what a method is
// stays one thing to read and gains a field rather than an argument.
typedef struct InstalledMethod {
    // The instantiation the method is declared on, and what it answers to.
    const Type *on;
    const char *name;

    // The body, as a GabExternFn. A void pointer because what a C body is
    // belongs to the VM's header, which this one is reached from rather than
    // reaching.
    void *body;

    const Type *receiver;
    const Type *result;

    // What the caller writes, the receiver excluded: it is parameter zero and is
    // named above rather than being the first of these.
    const Type *const *params;
    size_t param_count;
} InstalledMethod;

typedef void (*MethodInstaller)(void *ctx, const InstalledMethod *method);

// Registered once by whatever provides the bodies, so that a 'Vec<T>' first
// named by a later compile is declared the same way.
void type_registry_set_method_installer(TypeRegistry *registry, MethodInstaller install, void *ctx);

// Declares a method, or fails if the type already answers that name. Finds one
// by following 'owner' when the type itself does not answer, so that a type
// sharing another's identity reaches its set.
bool type_registry_add_method(TypeRegistry *registry, const Type *type, String *name, Symbol *method);
Symbol *type_registry_find_method(TypeRegistry *registry, const Type *type, const String *name);

/*
    Declaring a type in two steps, for the one caller that cannot do it in one.

    A struct's fields may name types not yet declared, so its name has to be
    bound before they resolve: 'struct A { b: box B }' is legal, and B may come
    later in the unit. The name is therefore opened here and the fields added as
    each resolves with type_add_field, and type_registry_complete settling the
    layout once they all have.

    A provider that knows its fields up front -- the standard library, and a host
    -- states them at once with type_registry_declare instead, which is these
    three calls in order.

    Between the two, the type has no layout: what may be done with it is what
    needs no width, which is to name it and to point at it.
*/
Type *type_registry_open_struct(TypeRegistry *registry, String *name, size_t max_fields);

// Lays the type out and settles how it frees, which is what finishes it. Called
// once every field is added; a type whose fields did not resolve is withdrawn
// instead of completed.
void type_registry_complete(TypeRegistry *registry, Type *type);

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

// 'primitive_names' are the names the built-in types are registered under, in
// the order TypeKind declares them -- interned by the caller, which owns the
// pool. The registry interns nothing itself: every name it is given is already a
// String *, so it holds types and not the pool that named them.
TypeRegistry *type_registry_create(Arena *arena, const TypePrimitiveNames *names);

// The names the language gives its own primitives, interned in 'strings'. What
// every registry over a Gab program is created with; a caller wanting other
// names fills the struct itself.
TypePrimitiveNames type_primitive_names(StringPool *strings);

void type_registry_destroy(TypeRegistry *registry);

// One of the types the language is written in terms of, by its kind. Every
// caller that needs 'int' or 'str' asks here rather than reaching for a field,
// which is what lets the registry's own state stay private.
const Type *type_registry_get_primitive(TypeRegistry *registry, TypeKind kind);

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
