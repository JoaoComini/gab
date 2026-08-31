#ifndef GAB_TYPE_REGISTRY_H
#define GAB_TYPE_REGISTRY_H

#include "arena.h"
#include "string/string.h"
#include "type.h"
#include "type_layout.h"
#include "util/hash_map.h"

#include <stdbool.h>

#define TYPE_REGISTRY_INITIAL_CAPACITY 8

typedef struct TypeRegistry TypeRegistry;

typedef struct TypePrimitiveNames {
    String *int_name;
    String *float_name;
    String *bool_name;
    String *byte_name;
    String *str_name;
    String *error_name;
} TypePrimitiveNames;

typedef struct Binding Binding;

typedef struct {
    const Type *type;

    const TypeDecl *decl;
} TypeBinding;

#define type_map_hash(key) (size_t)key
#define type_map_key_equals(key, other) key == other
#define type_map_key_dup(key) key

GAB_HASH_MAP(TypeMap, type_map, String *, TypeBinding)

typedef struct TypeDeclSpec {
    const TypeDecl *decl;

    const Type *derefs_to;
    const LentPart *lent_parts;
    size_t lent_part_count;
} TypeDeclSpec;

typedef struct TypeFieldSpec {
    String *name;
    const Type *type;
} TypeFieldSpec;

const Type *type_registry_declare_struct(TypeRegistry *registry, String *name, const TypeFieldSpec *fields,
                                         size_t field_count);

typedef struct Deref {
    const Type *to;

    LentPart parts[GAB_MAX_LENT_PARTS];
    size_t part_count;
} Deref;

bool type_registry_declare_owned(TypeRegistry *registry, const Type *type, Function *function);

Function *type_registry_find_owned(TypeRegistry *registry, const Type *type, const String *name);

/* True when a declaration's signature names no type parameter, so every instantiation shares it. */
bool type_registry_owned_is_shared(const Function *declaration, const Type *type);

void type_registry_complete(TypeRegistry *registry, const Type *type);

const TypeFields *type_registry_fields_of(TypeRegistry *registry, const Type *type);

const TypeField *type_registry_find_field(TypeRegistry *registry, const Type *type, const String *name);

bool type_registry_holds_its_memory_inline(TypeRegistry *registry, const Type *type);

bool type_registry_owns(TypeRegistry *registry, const Type *type);

/* True when a value of this type names memory it does not own, at any depth. */
bool type_registry_borrows(TypeRegistry *registry, const Type *type);

bool type_registry_copies(TypeRegistry *registry, const Type *type);

const DropPlan *type_registry_drop_of(TypeRegistry *registry, const Type *type);

const TypeLayout *type_registry_layout_of(TypeRegistry *registry, const Type *type);

size_t type_registry_size_of(TypeRegistry *registry, const Type *type);
size_t type_registry_align_of(TypeRegistry *registry, const Type *type);

TypeRegistry *type_registry_create(Arena *arena, const TypePrimitiveNames *names);

TypePrimitiveNames type_primitive_names(StringPool *strings);

void type_registry_destroy(TypeRegistry *registry);

const Type *type_registry_get_primitive(TypeRegistry *registry, TypeKind kind);

const Type *type_registry_declare(TypeRegistry *registry, const TypeDeclSpec *spec);

void type_registry_set_deref(TypeRegistry *registry, const Type *from, const Type *to, const LentPart *parts,
                             size_t part_count);

const Type *type_registry_deref_of(TypeRegistry *registry, const Type *type);

const Deref *type_registry_deref(TypeRegistry *registry, const Type *type);

const Type *type_registry_error_type(TypeRegistry *registry);

const Type *type_registry_array_of(TypeRegistry *registry, const Type *element, int32_t length);

const Type *type_registry_box_to(TypeRegistry *registry, const Type *inner);
const Type *type_registry_ref_to(TypeRegistry *registry, const Type *inner);

const Type *type_registry_ptr_to(TypeRegistry *registry, const Type *pointee);

const Type *type_registry_param(TypeRegistry *registry, size_t index);

const Type *type_registry_instantiate(TypeRegistry *registry, const TypeDecl *decl, const TypeArg *args,
                                      size_t arg_count);

const Type *type_registry_apply(TypeRegistry *registry, const TypeDecl *decl, const Type *const *args,
                                size_t arg_count);

const Type *type_registry_block_of(TypeRegistry *registry, const Type *element);

const Type *type_registry_substitute(TypeRegistry *registry, const Type *type, const Type *const *args,
                                     size_t arg_count);

#endif
