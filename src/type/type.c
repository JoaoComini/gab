#include "type_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(int32_t) == 4, "gab int must be 4 bytes");
_Static_assert(sizeof(float) == 4, "gab float must be 4 bytes");

Type type_init(TypeKind kind, String *name) {
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

Type *type_create(Arena *arena, TypeKind kind, String *name) {
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
    case TYPE_PTR:
    case TYPE_BLOCK:
        return type->indirect.pointee;

    default:
        return NULL;
    }
}

bool type_is_indirect(const Type *type) { return type && (type->kind == TYPE_BOX || type->kind == TYPE_REF); }

bool type_owns_through_an_address(const Type *type) {
    return type && (type->kind == TYPE_BOX || type->kind == TYPE_BLOCK);
}

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

    return type->args[1].constant.value;
}

bool type_array_length_is_known(const Type *type) {
    return type && type->kind == TYPE_ARRAY && type->arg_count == 2 && type->args[1].kind == TYPE_ARG_CONST &&
           type->args[1].constant.kind == CONST_VALUE;
}

TypeKind type_kind(const Type *type) { return type->kind; }

String *type_name_of(const Type *type) { return type->name; }

const TypeDecl *type_decl(const Type *type) { return type->decl; }

size_t type_arg_hash(TypeArg arg) {
    if (arg.kind == TYPE_ARG_TYPE) {
        return (size_t)(uintptr_t)arg.type;
    }

    return arg.constant.kind == CONST_PARAM ? arg.constant.param : (size_t)(uint32_t)arg.constant.value;
}

bool type_arg_equals(TypeArg arg, TypeArg other) {
    if (arg.kind != other.kind) {
        return false;
    }

    if (arg.kind == TYPE_ARG_TYPE) {
        return arg.type == other.type;
    }

    return arg.constant.kind == other.constant.kind && arg.constant.value == other.constant.value;
}

size_t type_structural_hash(const Type *type) {
    size_t hash = 5381;

    hash = ((hash << 5) + hash) + (size_t)type->kind;

    switch (type->kind) {
    case TYPE_BOX:
    case TYPE_REF:
    case TYPE_PTR:
    case TYPE_BLOCK:
        hash = ((hash << 5) + hash) + (size_t)(uintptr_t)type->indirect.pointee;
        break;

    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_STRUCT:
        hash = ((hash << 5) + hash) + (size_t)(uintptr_t)type->decl;

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
    case TYPE_PTR:
    case TYPE_BLOCK:
        return type->indirect.pointee == other->indirect.pointee;

    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_STRUCT:
        if (type->decl != other->decl || type->arg_count != other->arg_count) {
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
    case TYPE_INT:
    case TYPE_FLOAT:
    case TYPE_BOOL:
    case TYPE_BYTE:
    case TYPE_STR:
    case TYPE_SLICE:
    case TYPE_ARRAY:
        return true;
    default:
        return false;
    }
}

bool type_names_itself(const Type *type) {
    switch (type->kind) {
    case TYPE_INT:
    case TYPE_FLOAT:
    case TYPE_BOOL:
    case TYPE_BYTE:
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
