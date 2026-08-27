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

// Declared for the same reason: a scope owns a registry and names what it holds.
typedef struct Scope Scope;

// The named types of one scope. Scope owns these and chains them, the same way
// it chains symbol tables.
typedef struct {
    TypeHandle type;
} TypeBinding;

#define type_map_hash(key) (size_t)key
#define type_map_key_equals(key, other) key == other
#define type_map_key_dup(key) key

GAB_HASH_MAP(TypeMap, type_map, String *, TypeBinding)

// Every constructed type is interned on the application that built it, so that
// two mentions of 'box T' or of 'Array int,3' yield the same Type *: the whole
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

typedef struct {
    TypeHandle int_type;

    // One byte, which is what a string's characters are. Not spellable in the
    // language: it exists so that the pointer naming those characters carries a
    // stride, the way every other 'ptr T' does.
    TypeHandle byte_type;
    TypeHandle float_type;
    TypeHandle bool_type;
    TypeHandle string_type;

    // 'str'. The characters of a string, borrowed: the same two slots as a
    // 'String' and copied like one, owning nothing. A distinct interned Type
    // from the owning one because ownership is read off the type, and a literal
    // and a concatenation must not answer it the same way.
    //
    // Not what 'ref String' names. That is an indirection to a slot holding a
    // header, which is what 'ref' builds for every type in the language.
    TypeHandle str_type;

    // 'Array', the bare name. Not a usable type on its own -- every array is
    // 'Array T' for some element -- but the name a spec resolves to before its
    // element is applied, and what a diagnostic prints when one is missing.
    TypeHandle array_type;

    TypeHandle error_type;
} TypeBuiltins;

// Interning, not naming. One registry per VM holds the builtins and every
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
    TypeHandle type;
    const String *name;
} MethodKey;

#define method_key_hash(key) (((size_t)(key).type * 31) ^ (size_t)(key).name)
#define method_key_key_equals(key, other) ((key).type == (other).type && (key).name == (other).name)
#define method_key_key_dup(key) key
#define method_key_entry_free(key, value)

GAB_HASH_MAP(MethodTable, method_key, MethodKey, Symbol *)

// A nominal type's identity: the scope it was declared in and the name it was
// given. Not its shape -- two modules each declaring a 'Config' declare two
// types, however alike their fields.
typedef struct NominalKey {
    const Scope *scope;
    const String *name;
} NominalKey;

#define nominal_key_hash(key) (((size_t)(key).scope * 31) ^ (size_t)(key).name)
#define nominal_key_key_equals(key, other) ((key).scope == (other).scope && (key).name == (other).name)
#define nominal_key_key_dup(key) key
#define nominal_key_entry_free(key, value)

GAB_HASH_MAP(NominalMap, nominal_key, NominalKey, Type *)

typedef struct TypeRegistry {
    Arena *arena;

    StringPool *strings;

    // Every type built by applying a constructor: 'box T', 'ref T', 'ptr T',
    // 'Array T,N', and whatever a generic declaration adds. Keyed by the
    // application, so the constructor is part of what is looked up and two
    // constructors given the same argument never collide.
    TypeAppMap *applications;

    // Every method declared on every type, however far apart the declarations
    // were.
    MethodTable *methods;

    // Every nominal type a unit declared, by the declaration that names it.
    NominalMap *nominals;

    TypeBuiltins builtins;
} TypeRegistry;

// Declares a method, or fails if the type already answers that name. Finds one
// by following 'owner' when the type itself does not answer, so that a type
// sharing another's identity reaches its set.
bool type_registry_add_method(TypeRegistry *registry, TypeHandle type, String *name, Symbol *method);
Symbol *type_registry_find_method(TypeRegistry *registry, TypeHandle type, const String *name);

// Declares a nominal type, and finishes one.
//
// Two calls rather than one because a struct is reachable by name before its
// fields resolve -- that is what lets a field name the struct it is declared in
// -- so there is a window where the type exists without a layout. The window is
// here rather than in the resolver, which is the whole of why a type cannot be
// built anywhere else.
//
// Identity is the declaration: the scope it was declared in and the name it was
// given, so two modules each declaring a 'Config' declare two types however
// alike their fields.
Type *type_registry_declare_struct(TypeRegistry *registry, const Scope *scope, String *name,
                                   size_t max_fields);
void type_registry_finish_struct(TypeRegistry *registry, Type *type);
TypeHandle type_registry_find_struct(TypeRegistry *registry, const Scope *scope, String *name);

// A builtin, by the name it goes by. Declared under no scope, since it belongs
// to no module.
TypeHandle type_registry_find_builtin(TypeRegistry *registry, String *name);

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings);

void type_registry_destroy(TypeRegistry *registry);

TypeHandle type_registry_get_builtin(TypeRegistry *registry, TypeKind type);
TypeHandle type_registry_error_type(TypeRegistry *registry);

// The interned 'Array element': a header of {data, length} owning a block of
// elements. One per element type, and the type that carries the element -- the
// raw address naming the block does not, so this is what supplies the walk that
// frees them.
TypeHandle type_registry_array_of(TypeRegistry *registry, TypeHandle element, int32_t length);

// The interned 'box inner' and 'ref inner'. Two constructors rather than one
// taking a flag: which of them a type is decides whether a slot holding it
// frees what it names, and that is the question the kind exists to answer.
TypeHandle type_registry_box_to(TypeRegistry *registry, TypeHandle inner);
TypeHandle type_registry_ref_to(TypeRegistry *registry, TypeHandle inner);

// The interned 'ptr pointee': an address the size of a machine pointer, owning
// nothing. Distinct from both indirections of the same pointee, since what a
// slot must free is read off the type.
TypeHandle type_registry_ptr_to(TypeRegistry *registry, TypeHandle pointee);

#endif
