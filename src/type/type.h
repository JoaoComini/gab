#ifndef GAB_TYPE_H
#define GAB_TYPE_H

#include "allocator.h"
#include "arena.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "util/hash_map.h"
#include "util/list.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_BOOL,

    TYPE_BYTE,

    TYPE_PTR,

    TYPE_STR,

    TYPE_ARRAY,
    TYPE_SLICE,
    TYPE_STRUCT,

    TYPE_BOX,
    TYPE_REF,

    TYPE_BLOCK,

    TYPE_PARAM,

    TYPE_UNKNOWN,
    TYPE_ERROR,
} TypeKind;

#define GAB_MAX_TYPE_BYTES 255

#define GAB_MAX_TYPE_PARAMS 4

#define GAB_MAX_DROP_STEPS 16

#define GAB_MAX_METHOD_PARAMS 8

typedef struct Type Type;

typedef struct TypeRegistry TypeRegistry;

/* A value, as 'Type' is a type; every one is an 'int', so a second const type must be recorded here. */
typedef struct TypeConst {
    enum {
        CONST_VALUE,
        CONST_PARAM,
    } kind;

    union {
        int32_t value;
        size_t param;
    };
} TypeConst;

/* The kind says which axis a parameter sits on and never changes; the payload says whether it is fixed. */
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

typedef struct Binding Binding;

typedef struct Function Function;

typedef struct TypeField {
    String *name;
    const Type *type;
} TypeField;

typedef struct TypeFields {
    const TypeField *fields;
    size_t count;
} TypeFields;

typedef enum {
    BODY_GAB,

    BODY_HOST,

    /* The compiler lowers the call itself, so nothing is bound and no body is written. */
    BODY_INTRINSIC,

} BodyKind;

typedef struct TypeDecl {
    String *name;

    size_t param_count;

    const TypeField *fields;
    size_t field_count;
} TypeDecl;

TypeKind type_kind(const Type *type);
String *type_name_of(const Type *type);

const TypeDecl *type_decl(const Type *type);

const TypeArg *type_args(const Type *type);
size_t type_arg_count(const Type *type);

/* An argument slot inference has not fixed yet: a type argument with no type. */
#define TYPE_ARG_NONE ((TypeArg){.kind = TYPE_ARG_TYPE, .type = NULL})

static inline bool type_arg_is_set(TypeArg arg) { return arg.kind != TYPE_ARG_TYPE || arg.type != NULL; }

/* An argument is hashed and compared by its kind and payload, never as bytes: the union has padding. */
size_t type_arg_hash(TypeArg arg);
bool type_arg_equals(TypeArg arg, TypeArg other);

size_t type_structural_hash(const Type *type);
bool type_structurally_equals(const Type *type, const Type *other);

GAB_LIST(TypeList, type_list, const Type *)

const Type *type_pointee(const Type *type);

size_t type_param_index(const Type *type);

bool type_has_param(const Type *type);

Type *type_create(Arena *arena, TypeKind kind, String *name);

Type type_init(TypeKind kind, String *name);

TypeMetadata type_metadata_of(const Type *type);

bool type_is_str_ref(const Type *type);

bool type_is_sized(const Type *type);

bool type_is_primitive(const Type *type);

bool type_names_itself(const Type *type);

bool type_is_indirect(const Type *type);

bool type_owns_through_an_address(const Type *type);

const Type *type_array_element(const Type *type);
int32_t type_array_length(const Type *type);

/* False while the length is still a parameter, so the array has no width yet. */
bool type_array_length_is_known(const Type *type);

const Type *type_slice_element(const Type *type);

typedef struct LentPart {
    size_t offset;
    size_t size;
} LentPart;

#define GAB_MAX_LENT_PARTS 4

#endif
