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

const TypeField *type_find_field(const Type *type, const String *name) {
    const TypeField *fields = type_fields(type);
    size_t count = type_field_count(type);

    for (size_t i = 0; i < count; i++) {
        if (fields[i].name == name) {
            return &fields[i];
        }
    }

    return NULL;
}

TypeMetadata type_metadata_of(const Type *type) {
    if (!type) {
        return TYPE_META_NONE;
    }

    switch (type->kind) {
    case TYPE_STR:
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

const TypeField *type_fields(const Type *type) {
    if (!type) {
        return NULL;
    }

    switch (type->kind) {
    case TYPE_STRUCT:
        return type->record.substituted ? type->record.substituted->fields : type->decl->fields;

    default:
        return NULL;
    }
}

size_t type_field_count(const Type *type) {
    if (!type) {
        return 0;
    }

    switch (type->kind) {
    case TYPE_STRUCT:
        return type->record.substituted ? type->record.substituted->count : type->decl->field_count;

    default:
        return 0;
    }
}

bool type_is_indirect(const Type *type) { return type && (type->kind == TYPE_BOX || type->kind == TYPE_REF); }

bool type_owns_through_an_address(const Type *type) {
    return type && (type->kind == TYPE_BOX || type->kind == TYPE_BLOCK);
}

bool type_holds_its_memory_inline(const Type *type) {
    for (size_t i = 0; i < type_field_count(type); i++) {
        if (type_fields(type)[i].type && type_fields(type)[i].type->kind == TYPE_BLOCK) {
            return true;
        }
    }

    return false;
}

const Type *type_array_element(const Type *type) {
    assert(type && type->kind == TYPE_ARRAY && "only an array has an element");

    return type->array.element;
}

int32_t type_array_length(const Type *type) {
    assert(type && type->kind == TYPE_ARRAY && "only an array has a length");

    return type->array.length;
}

bool type_is_owned(const Type *type) {
    if (!type) {
        return false;
    }

    switch (type->kind) {
    case TYPE_BOX:
        return true;
    case TYPE_REF:
        return false;

    case TYPE_BLOCK:
        return true;

    case TYPE_PTR:
        return false;

    case TYPE_ARRAY:
        return type_is_owned(type_array_element(type));

    case TYPE_STR:
        return false;

    default:
        break;
    }

    for (size_t i = 0; i < type_field_count(type); i++) {
        if (type_is_owned(type_fields(type)[i].type)) {
            return true;
        }
    }

    return false;
}

bool type_is_copyable(const Type *type) {
    if (!type) {
        return true;
    }

    switch (type->kind) {
    case TYPE_BOX:
        return false;
    case TYPE_REF:
    case TYPE_PTR:
        return true;

    case TYPE_BLOCK:
        return false;

    case TYPE_ARRAY:
        return type_is_copyable(type_array_element(type));

    case TYPE_STR:
        return true;

    default:
        break;
    }

    for (size_t i = 0; i < type_field_count(type); i++) {
        if (!type_is_copyable(type_fields(type)[i].type)) {
            return false;
        }
    }

    return true;
}

TypeKind type_kind(const Type *type) { return type->kind; }

String *type_name_of(const Type *type) { return type->name; }

const TypeDef *type_decl(const Type *type) { return type->decl; }

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
        hash = ((hash << 5) + hash) + (size_t)(uintptr_t)type->array.element;
        hash = ((hash << 5) + hash) + (size_t)(uint32_t)type->array.length;
        break;

    case TYPE_STRUCT:
        hash = ((hash << 5) + hash) + (size_t)(uintptr_t)type->decl;

        for (size_t i = 0; i < type->arg_count; i++) {
            const TypeArg *arg = &type->args[i];

            hash = ((hash << 5) + hash) + (size_t)arg->kind;

            hash = ((hash << 5) + hash) +
                   (arg->kind == TYPE_ARG_TYPE ? (size_t)(uintptr_t)arg->type : (size_t)(uint32_t)arg->value);
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
        return type->array.element == other->array.element && type->array.length == other->array.length;

    case TYPE_STRUCT:
        if (type->decl != other->decl || type->arg_count != other->arg_count) {
            return false;
        }

        for (size_t i = 0; i < type->arg_count; i++) {
            if (type->args[i].kind != other->args[i].kind) {
                return false;
            }

            if (type->args[i].kind == TYPE_ARG_TYPE) {
                if (type->args[i].type != other->args[i].type) {
                    return false;
                }
            } else if (type->args[i].value != other->args[i].value) {
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
