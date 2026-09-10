#ifndef GAB_TYPE_H
#define GAB_TYPE_H

#include "decl_id.h"
#include "memory/allocator.h"
#include "memory/arena.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "util/hash_map.h"
#include "util/list.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TYPE_I32,
    TYPE_F32,
    TYPE_BOOL,

    TYPE_U8,
    TYPE_USIZE,

    TYPE_RAW,

    TYPE_STR,

    TYPE_ARRAY,
    TYPE_SLICE,
    TYPE_STRUCT,

    TYPE_BOX,
    TYPE_REF,

    TYPE_PARAM,

    TYPE_UNKNOWN,
    TYPE_ERROR,
} TypeKind;

#include "constant.h"

#define GAB_MAX_TYPE_PARAMS 4

#define GAB_MAX_DROP_STEPS 16

#define GAB_MAX_METHOD_PARAMS 8

typedef struct Type Type;

typedef struct TypeRegistry TypeRegistry;

typedef struct TypeConst {
    enum {
        CONST_VALUE,
        CONST_PARAM,
    } kind;

    union {
        Constant value;
        size_t param;
    };
} TypeConst;

typedef struct TypeArg {
    enum {
        TYPE_ARG_TYPE,
        TYPE_ARG_CONST,
    } kind;

    union {
        const Type *type;
        TypeConst constant;
    };
} TypeArg;

typedef enum {
    TYPE_META_NONE,

    TYPE_META_LENGTH,
} TypeMetadata;

typedef struct TypeField {
    String *name;
    const Type *type;
} TypeField;

typedef struct TypeFields {
    const TypeField *fields;
    size_t count;
} TypeFields;

typedef struct TypeMemberKey {
    DeclId owner;

    const String *name;
} TypeMemberKey;

TypeMemberKey type_member_key_of(const Type *type, const String *name);

typedef struct InstanceId {
    DeclId decl;

    TypeArg args[GAB_MAX_TYPE_PARAMS];
    size_t arg_count;
} InstanceId;

InstanceId instance_id_of(DeclId decl, const TypeArg *args, size_t arg_count);

bool instance_id_equals(InstanceId id, InstanceId other);
size_t instance_id_hash(InstanceId id);

typedef struct ConformanceKey {
    DeclId owner;

    InstanceId interface;
} ConformanceKey;

ConformanceKey conformance_key_of(const Type *type, DeclId interface, const TypeArg *args, size_t arg_count);

bool conformance_key_equals(ConformanceKey key, ConformanceKey other);
size_t conformance_key_hash_of(ConformanceKey key);

typedef struct TypeDecl {
    DeclId id;

    size_t param_count;

    const TypeField *fields;
    size_t field_count;
} TypeDecl;

TypeKind type_kind(const Type *type);
const String *type_name_of(const Type *type);

const TypeDecl *type_decl(const Type *type);

const TypeArg *type_args(const Type *type);
size_t type_arg_count(const Type *type);

#define TYPE_ARG_NONE ((TypeArg){.kind = TYPE_ARG_TYPE, .type = NULL})

static inline bool type_arg_is_set(TypeArg arg) { return arg.kind != TYPE_ARG_TYPE || arg.type != NULL; }

size_t type_arg_hash(TypeArg arg);
bool type_arg_equals(TypeArg arg, TypeArg other);

size_t type_structural_hash(const Type *type);
bool type_structurally_equals(const Type *type, const Type *other);

GAB_LIST(TypeList, type_list, const Type *)

const Type *type_pointee(const Type *type);

size_t type_param_index(const Type *type);

bool type_has_param(const Type *type);

Type *type_create(Arena *arena, TypeKind kind, const String *name);

Type type_init(TypeKind kind, const String *name);

TypeMetadata type_metadata_of(const Type *type);

bool type_is_str_ref(const Type *type);

bool type_is_sized(const Type *type);

bool type_is_primitive(const Type *type);

bool type_is_integer(const Type *type);

bool type_is_unsigned(const Type *type);

bool type_names_itself(const Type *type);

bool type_is_indirect(const Type *type);

const Type *type_array_element(const Type *type);
int32_t type_array_length(const Type *type);

bool type_array_length_is_known(const Type *type);

const Type *type_slice_element(const Type *type);

#endif
