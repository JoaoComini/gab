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

typedef struct KnownNames {
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

    DeclId destroy_interface;

    String *destroy_method;
    String *unique;
    String *index;
    String *len;
    String *as_bytes;

    String *caller;
    String *size_of;
} KnownNames;

typedef struct TypeFieldSpec {
    String *name;
    const Type *type;
} TypeFieldSpec;

const Type *type_registry_declare_struct(TypeRegistry *registry, String *name, const TypeFieldSpec *fields,
                                         size_t field_count);

bool type_registry_declare_conformance(TypeRegistry *registry, const Type *type, DeclId interface,
                                       const TypeArg *args, size_t arg_count);

bool type_registry_conforms(TypeRegistry *registry, const Type *type, DeclId interface, const TypeArg *args,
                            size_t arg_count);

bool type_registry_conforms_at_any(TypeRegistry *registry, const Type *type, DeclId interface);

void type_registry_complete(TypeRegistry *registry, const Type *type);

const TypeFields *type_registry_fields_of(TypeRegistry *registry, const Type *type);

const TypeField *type_registry_find_field(TypeRegistry *registry, const Type *type, const String *name);

bool type_registry_owns(TypeRegistry *registry, const Type *type);

bool type_registry_borrows(TypeRegistry *registry, const Type *type);

bool type_registry_copies(TypeRegistry *registry, const Type *type);

const TypeLayout *type_registry_layout_of(TypeRegistry *registry, const Type *type);

size_t type_registry_size_of(TypeRegistry *registry, const Type *type);
size_t type_registry_align_of(TypeRegistry *registry, const Type *type);

TypeRegistry *type_registry_create(Arena *arena, const KnownNames *names);

KnownNames known_names(StringPool *strings);

const IntrinsicLowering *type_registry_intrinsic(const TypeRegistry *registry, const String *owner,
                                                 const String *name);

const KnownNames *type_registry_names(const TypeRegistry *registry);

bool type_registry_is_unique(const TypeRegistry *registry, const Type *type);

void type_registry_destroy(TypeRegistry *registry);

const Type *type_registry_get_primitive(TypeRegistry *registry, TypeKind kind);

const Type *type_registry_declare(TypeRegistry *registry, const TypeDecl *decl);

const Type *type_registry_deref_of(TypeRegistry *registry, const Type *type);

const Type *type_registry_error_type(TypeRegistry *registry);

const Type *type_registry_array_of(TypeRegistry *registry, const Type *element, int32_t length);

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
