#include "type_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(int32_t) == 4, "gab int must be 4 bytes");
_Static_assert(sizeof(float) == 4, "gab float must be 4 bytes");

Type type_init(TypeKind kind, const String *name) {
    Type type;

    type.kind = kind;
    type.name = name;
    type.decl = NULL;
    type.args = NULL;
    type.arg_count = 0;

    type.has_param = kind == TYPE_PARAM;

    memset(&type.record, 0, sizeof(type.record));

    return type;
}

Type *type_create(Arena *arena, TypeKind kind, const String *name) {
    Type *type = arena_alloc(arena, sizeof(Type));

    *type = type_init(kind, name);

    return type;
}

TypeMetadata type_metadata_of(const Type *type) {
    if (!type) {
        return TYPE_META_NONE;
    }

    switch (type->kind) {
    case TYPE_STR:
    case TYPE_SLICE:
        return TYPE_META_LENGTH;

    default:
        return TYPE_META_NONE;
    }
}

bool type_is_str_ref(const Type *type) {
    return type && type->kind == TYPE_REF && type->indirect.pointee &&
           type->indirect.pointee->kind == TYPE_STR;
}

bool type_is_sized(const Type *type) {
    if (!type) {
        return true;
    }

    switch (type->kind) {
    case TYPE_STR:
    case TYPE_SLICE:
        return false;

    case TYPE_PARAM:
        return false;

    default:
        return true;
    }
}

const Type *type_pointee(const Type *type) {
    if (!type) {
        return NULL;
    }

    switch (type->kind) {
    case TYPE_BOX:
    case TYPE_REF:
    case TYPE_RAW:
        return type->indirect.pointee;

    default:
        return NULL;
    }
}

bool type_is_indirect(const Type *type) { return type && (type->kind == TYPE_BOX || type->kind == TYPE_REF); }

const Type *type_array_element(const Type *type) {
    assert(type && type->kind == TYPE_ARRAY && "only an array has an element");
    assert(type->arg_count == 2 && type->args[0].kind == TYPE_ARG_TYPE && "an array is element and length");

    return type->args[0].type;
}

const Type *type_slice_element(const Type *type) {
    assert(type && type->kind == TYPE_SLICE && "only a slice has an element");
    assert(type->arg_count == 1 && type->args[0].kind == TYPE_ARG_TYPE && "a slice is one element");

    return type->args[0].type;
}

int32_t type_array_length(const Type *type) {
    assert(type && type->kind == TYPE_ARRAY && "only an array has a length");
    assert(type_array_length_is_known(type) && "a generic length is no count until it is substituted");

    return type->args[1].constant.value.as_int;
}

bool type_array_length_is_known(const Type *type) {
    return type && type->kind == TYPE_ARRAY && type->arg_count == 2 && type->args[1].kind == TYPE_ARG_CONST &&
           type->args[1].constant.kind == CONST_VALUE;
}

TypeKind type_kind(const Type *type) { return type->kind; }

const String *type_name_of(const Type *type) { return type->name; }

const TypeDecl *type_decl(const Type *type) { return type->decl; }

size_t type_arg_hash(TypeArg arg) {
    if (arg.kind == TYPE_ARG_TYPE) {
        return (size_t)(uintptr_t)arg.type;
    }

    return arg.constant.kind == CONST_PARAM ? arg.constant.param : constant_hash(arg.constant.value);
}

bool type_arg_equals(TypeArg arg, TypeArg other) {
    if (arg.kind != other.kind) {
        return false;
    }

    if (arg.kind == TYPE_ARG_TYPE) {
        return arg.type == other.type;
    }

    if (arg.constant.kind != other.constant.kind) {
        return false;
    }

    /* A parameter is compared by its index and a value by its value; the union holds one or the other. */
    return arg.constant.kind == CONST_PARAM ? arg.constant.param == other.constant.param
                                            : constant_equals(arg.constant.value, other.constant.value);
}

size_t type_structural_hash(const Type *type) {
    size_t hash = 5381;

    hash = ((hash << 5) + hash) + (size_t)type->kind;

    switch (type->kind) {
    case TYPE_BOX:
    case TYPE_REF:
    case TYPE_RAW:
        hash = ((hash << 5) + hash) + (size_t)(uintptr_t)type->indirect.pointee;
        break;

    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_STRUCT:
        hash = ((hash << 5) + hash) + decl_id_hash(type->decl->id);

        for (size_t i = 0; i < type->arg_count; i++) {
            const TypeArg *arg = &type->args[i];

            hash = ((hash << 5) + hash) + (size_t)arg->kind;

            hash = ((hash << 5) + hash) + type_arg_hash(*arg);
        }
        break;

    default:

        break;
    }

    return hash;
}

bool type_structurally_equals(const Type *type, const Type *other) {
    if (type->kind != other->kind) {
        return false;
    }

    switch (type->kind) {
    case TYPE_BOX:
    case TYPE_REF:
    case TYPE_RAW:
        return type->indirect.pointee == other->indirect.pointee;

    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_STRUCT:
        if (!decl_id_equals(type->decl->id, other->decl->id) || type->arg_count != other->arg_count) {
            return false;
        }

        for (size_t i = 0; i < type->arg_count; i++) {
            if (!type_arg_equals(type->args[i], other->args[i])) {
                return false;
            }
        }

        return true;

    default:
        return type == other;
    }
}

const TypeArg *type_args(const Type *type) { return type->args; }

size_t type_arg_count(const Type *type) { return type->arg_count; }

bool type_is_primitive(const Type *type) {
    switch (type->kind) {
    case TYPE_I32:
    case TYPE_F32:
    case TYPE_BOOL:
    case TYPE_U8:
    case TYPE_USIZE:
    case TYPE_STR:
    case TYPE_SLICE:
    case TYPE_ARRAY:
    case TYPE_RAW:
        return true;
    default:
        return false;
    }
}

/* Listing every kind rather than the ones that answer true, so a kind added later must be placed here. */
bool type_is_integer(const Type *type) {
    if (!type) {
        return false;
    }

    switch (type->kind) {
    case TYPE_I32:
    case TYPE_U8:
    case TYPE_USIZE:
        return true;

    case TYPE_F32:
    case TYPE_BOOL:
    case TYPE_STR:
    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_STRUCT:
    case TYPE_BOX:
    case TYPE_REF:
    case TYPE_RAW:
    case TYPE_PARAM:
    case TYPE_UNKNOWN:
    case TYPE_ERROR:
        return false;
    }

    return false;
}

/* Listing every kind rather than the ones that answer true, so a kind added later must be placed here. */
bool type_is_unsigned(const Type *type) {
    if (!type) {
        return false;
    }

    switch (type->kind) {
    case TYPE_U8:
    case TYPE_USIZE:
        return true;

    case TYPE_I32:
    case TYPE_F32:
    case TYPE_BOOL:
    case TYPE_STR:
    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_STRUCT:
    case TYPE_BOX:
    case TYPE_REF:
    case TYPE_RAW:
    case TYPE_PARAM:
    case TYPE_UNKNOWN:
    case TYPE_ERROR:
        return false;
    }

    return false;
}

bool type_names_itself(const Type *type) {
    switch (type->kind) {
    case TYPE_I32:
    case TYPE_F32:
    case TYPE_BOOL:
    case TYPE_U8:
    case TYPE_USIZE:
    case TYPE_STR:
    case TYPE_ERROR:

    case TYPE_PARAM:
        return true;
    default:
        return false;
    }
}

size_t type_param_index(const Type *type) {
    assert(type && type->kind == TYPE_PARAM && "only a parameter has an index");
    return type->param.index;
}

bool type_has_param(const Type *type) { return type && type->has_param; }

TypeMemberKey type_member_key_of(const Type *type, const String *name) {
    const TypeDecl *decl = type_decl(type);

    return (TypeMemberKey){.owner = decl ? decl->id : (DeclId){0}, .name = name};
}

ConformanceKey conformance_key_of(const Type *type, DeclId interface, const TypeArg *args, size_t arg_count) {
    const TypeDecl *decl = type_decl(type);

    ConformanceKey key = {.owner = decl ? decl->id : (DeclId){0}, .interface = interface};

    for (size_t i = 0; i < arg_count && i < GAB_MAX_TYPE_PARAMS; i++) {
        key.args[i] = args[i];
        key.arg_count++;
    }

    return key;
}

bool conformance_key_equals(ConformanceKey key, ConformanceKey other) {
    if (!decl_id_equals(key.owner, other.owner) || !decl_id_equals(key.interface, other.interface) ||
        key.arg_count != other.arg_count) {
        return false;
    }

    for (size_t i = 0; i < key.arg_count; i++) {
        if (!type_arg_equals(key.args[i], other.args[i])) {
            return false;
        }
    }

    return true;
}

size_t conformance_key_hash_of(ConformanceKey key) {
    size_t hash = decl_id_hash(key.owner) * 31 + decl_id_hash(key.interface);

    for (size_t i = 0; i < key.arg_count; i++) {
        hash = hash * 31 + type_arg_hash(key.args[i]);
    }

    return hash;
}
