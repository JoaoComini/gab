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

typedef struct TypeArg {
    enum {
        TYPE_ARG_TYPE,
        TYPE_ARG_CONST,
    } kind;

    union {
        const Type *type;
        int32_t value;
    };
} TypeArg;

typedef enum {
    TYPE_META_NONE,

    TYPE_META_LENGTH,
} TypeMetadata;

typedef struct Symbol Symbol;

typedef struct Function Function;

typedef struct TypeField {
    String *name;
    const Type *type;
} TypeField;

typedef struct TypeFields {
    const TypeField *fields;
    size_t count;
} TypeFields;

typedef struct MethodDecl {
    String *name;

    void *body;

    const Type *receiver;
    const Type *result;

    const Type *const *params;
    size_t param_count;

    Function *function;
} MethodDecl;

typedef struct TypeDef {
    String *name;

    size_t param_count;

    const TypeField *fields;
    size_t field_count;
} TypeDef;

TypeKind type_kind(const Type *type);
String *type_name_of(const Type *type);

const TypeDef *type_decl(const Type *type);

const TypeArg *type_args(const Type *type);
size_t type_arg_count(const Type *type);

size_t type_structural_hash(const Type *type);
bool type_structurally_equals(const Type *type, const Type *other);

#define type_list_item_free(item) ((void)(item))
GAB_LIST(TypeList, type_list, const Type *)

const Type *type_pointee(const Type *type);

size_t type_param_index(const Type *type);

bool type_has_param(const Type *type);

Type *type_create(Arena *arena, TypeKind kind, String *name);

Type type_init(TypeKind kind, String *name);

TypeMetadata type_metadata_of(const Type *type);

bool type_is_str_ref(const Type *type);

bool type_is_sized(const Type *type);

bool type_names_itself(const Type *type);

bool type_is_indirect(const Type *type);

bool type_owns_through_an_address(const Type *type);

const Type *type_array_element(const Type *type);
int32_t type_array_length(const Type *type);

typedef struct LentPart {
    size_t offset;
    size_t size;
} LentPart;

#define GAB_MAX_LENT_PARTS 4

#endif
