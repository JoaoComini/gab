#ifndef GAB_TYPE_REGISTRY_H
#define GAB_TYPE_REGISTRY_H

#include "memory/arena.h"
#include "string/string.h"
#include "type.h"
#include "type_layout.h"
#include "util/hash_map.h"

#include <stdbool.h>

#define TYPE_REGISTRY_INITIAL_CAPACITY 8

typedef struct TypeRegistry TypeRegistry;

typedef struct IntrinsicLowering IntrinsicLowering;

/* Every name the compiler knows, interned once so a site compares pointers rather than characters. */
typedef struct KnownNames {
    /* Spelled by the source itself, and fixed by the grammar. */
    String *i32;
    String *f32;
    String *boolean;
    String *u8;
    String *usize;
    String *str;
    String *slice;
    String *raw;
    String *array;
    String *self;
    String *error;

    /* Declared by the core and found by name, so the two drift if either moves alone. */
    String *destroy;
    String *destroy_method;
    String *unique;
    String *index;
    String *len;
    String *as_bytes;

    /* Written as '@name', which the compiler answers rather than binds. */
    String *caller;
    String *size_of;
} KnownNames;

typedef struct Binding Binding;

typedef struct {
    /* What the name binds: a type, or the value 'array<T, N>' names for its length. */
    TypeArg arg;

    const TypeDecl *decl;
} TypeBinding;

#define type_map_hash(key) (size_t)key
#define type_map_key_equals(key, other) key == other

GAB_HASH_MAP(TypeMap, type_map, String *, TypeBinding)

typedef struct TypeFieldSpec {
    String *name;
    const Type *type;
} TypeFieldSpec;

const Type *type_registry_declare_struct(TypeRegistry *registry, String *name, const TypeFieldSpec *fields,
                                         size_t field_count);

bool type_registry_declare_owned(TypeRegistry *registry, const Type *type, Function *function);

Function *type_registry_find_owned(TypeRegistry *registry, const Type *type, const String *name);

/* False when this type already implements the interface, which it may do only once. */
bool type_registry_declare_conformance(TypeRegistry *registry, const Type *type, const String *interface);

bool type_registry_conforms(TypeRegistry *registry, const Type *type, const String *interface);

/* What this type runs as it ends, or NULL where it declares no ending of its own. */
Function *type_registry_destructor(TypeRegistry *registry, const Type *type);

/* True when a declaration's signature names no type parameter, so every instantiation shares it. */
bool type_registry_owned_is_shared(const Function *declaration, const Type *type);

void type_registry_complete(TypeRegistry *registry, const Type *type);

const TypeFields *type_registry_fields_of(TypeRegistry *registry, const Type *type);

const TypeField *type_registry_find_field(TypeRegistry *registry, const Type *type, const String *name);

bool type_registry_owns(TypeRegistry *registry, const Type *type);

/* True when a value of this type names memory it does not own, at any depth. */
bool type_registry_borrows(TypeRegistry *registry, const Type *type);

bool type_registry_copies(TypeRegistry *registry, const Type *type);

const TypeLayout *type_registry_layout_of(TypeRegistry *registry, const Type *type);

size_t type_registry_size_of(TypeRegistry *registry, const Type *type);
size_t type_registry_align_of(TypeRegistry *registry, const Type *type);

TypeRegistry *type_registry_create(Arena *arena, const KnownNames *names);

KnownNames known_names(StringPool *strings);

/* The lowering for a call the compiler expands rather than binds, none where the pair names no intrinsic. */
const IntrinsicLowering *type_registry_intrinsic(const TypeRegistry *registry, const String *owner,
                                                 const String *name);

/* The names the registry was built with, which every site matching one compares against by pointer. */
const KnownNames *type_registry_names(const TypeRegistry *registry);

/* Whether the type is the owner the compiler writes a drop for, rather than a struct that merely holds a run.
 */
bool type_registry_is_unique(const TypeRegistry *registry, const Type *type);

void type_registry_destroy(TypeRegistry *registry);

const Type *type_registry_get_primitive(TypeRegistry *registry, TypeKind kind);

const Type *type_registry_declare(TypeRegistry *registry, const TypeDecl *decl);

const Type *type_registry_deref_of(TypeRegistry *registry, const Type *type);

const Type *type_registry_error_type(TypeRegistry *registry);

/* Shared by every array, which is where the conformance all of them have is recorded. */

const Type *type_registry_array_of(TypeRegistry *registry, const Type *element, int32_t length);

/* An array whose length may still be a parameter, as 'array<T, N>' inside a generic declaration. */
const Type *type_registry_array_with(TypeRegistry *registry, const Type *element, TypeArg length);

const Type *type_registry_slice_of(TypeRegistry *registry, const Type *element);

const Type *type_registry_box_to(TypeRegistry *registry, const Type *inner);
const Type *type_registry_ref_to(TypeRegistry *registry, const Type *inner);

const Type *type_registry_raw_of(TypeRegistry *registry, const Type *pointee);

const Type *type_registry_param(TypeRegistry *registry, size_t index);

const Type *type_registry_instantiate(TypeRegistry *registry, const TypeDecl *decl, const TypeArg *args,
                                      size_t arg_count);

const Type *type_registry_apply(TypeRegistry *registry, const TypeDecl *decl, const Type *const *args,
                                size_t arg_count);

const Type *type_registry_substitute(TypeRegistry *registry, const Type *type, const TypeArg *args,
                                     size_t arg_count);

#endif
