#include "type.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(int32_t) == 4, "gab int must be 4 bytes");
_Static_assert(sizeof(float) == 4, "gab float must be 4 bytes");

Type *type_create(Arena *arena, TypeKind kind, String *name) {
    Type *type = arena_alloc(arena, sizeof(Type));
    type->kind = kind;
    type->name = name;
    type->decl = NULL;
    type->generic = NULL;

    // One arm zeroed is every arm zeroed: whichever the kind reads, it reads
    // nothing rather than whatever the arena held.
    memset(&type->record, 0, sizeof(type->record));

    return type;
}

Type *type_struct_create(Arena *arena, String *name, size_t max_fields) {
    Type *type = type_create(arena, TYPE_STRUCT, name);

    if (max_fields > 0) {
        type->record.fields = arena_alloc(arena, max_fields * sizeof(TypeField));
    }

    return type;
}

void type_add_field(Type *type, String *name, const Type *field_type) {
    assert(type->record.fields && "struct was created without room for fields");

    type->record.fields[type->record.field_count++] = (TypeField){
        .name = name,
        .type = field_type,
    };
}

const TypeField *type_find_field(const Type *type, const String *name) {
    for (size_t i = 0; i < type->record.field_count; i++) {
        const TypeField *field = &type->record.fields[i];

        if (field->name == name) {
            return field;
        }
    }

    return NULL;
}

size_t type_lent_fields(const Type *lender, const Type *pointee, const TypeField **out, size_t max) {
    // What a reference to characters carries. Named here because the pointee is
    // the characters themselves, which have no fields: a 'str' is a run of
    // bytes, and the address and count naming it belong to the reference.
    static const char *const of_characters[] = {"data", "length"};

    if (!pointee || pointee->kind != TYPE_STR) {
        return 0;
    }

    size_t count = 0;

    for (size_t i = 0; i < sizeof(of_characters) / sizeof(*of_characters) && count < max; i++) {
        for (size_t j = 0; j < type_field_count(lender); j++) {
            const TypeField *field = &type_fields(lender)[j];

            if (field->name && strcmp(field->name->data, of_characters[i]) == 0) {
                out[count++] = field;
                break;
            }
        }
    }

    return count;
}

// A raw pointer is deliberately not one of these. Every caller is a
// language-level path -- an auto-deref, a lend, a field access -- and a 'ptr T'
// is reachable from none of them: it names a block the header beside it
// describes, and reaching through it is that header's business.
TypeMetadata type_metadata_of(const Type *type) {
    if (!type) {
        return TYPE_META_NONE;
    }

    switch (type->kind) {
    // How far the characters run is not in their type, so a reference to them
    // carries that count beside the address.
    case TYPE_STR:
        return TYPE_META_LENGTH;

    // Named by a bare address, which is what all but the handful that say
    // otherwise are.
    default:
        return TYPE_META_NONE;
    }
}

bool type_is_str_ref(const Type *type) {
    return type && type->kind == TYPE_REF && type->indirect.pointee &&
           type->indirect.pointee->kind == TYPE_STR;
}

// Whether a value of this type can be held at all.
//
// Read off the kind rather than stored, because it is what the kind means: the
// characters of a string run however far they run, and no slot can reserve room
// for that. A type that named something unsized would be answering for its
// kind, not for itself.
bool type_is_sized(const Type *type) {
    if (!type) {
        return true;
    }

    switch (type->kind) {
    // The characters themselves, however many there are. Reached only through a
    // reference, which carries the count the type does not.
    case TYPE_STR:
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
    case TYPE_STRING:
        return type->record.fields;

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
    case TYPE_STRING:
        return type->record.field_count;

    default:
        return 0;
    }
}

bool type_is_indirect(const Type *type) { return type && (type->kind == TYPE_BOX || type->kind == TYPE_REF); }

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
    // The two indirections differ in exactly this, which is why they are two
    // kinds: one frees what it names and one does not.
    case TYPE_BOX:
        return true;
    case TYPE_REF:
        return false;

    // The memory is the value's own, and freeing it is what holding one means.
    case TYPE_BLOCK:
        return true;

    // A raw address claims nothing about what it names, so nothing frees
    // through one. The header naming a block is what owns it.
    case TYPE_PTR:
        return false;

    // An array owns exactly what its elements do: it is a run of them and holds
    // nothing else, so the element answers for the whole run however long it is.
    case TYPE_ARRAY:
        return type_is_owned(type_array_element(type));

    // A string owns its characters, always: what borrows them is a 'ref str',
    // and no reference owns anything. So the kind is the whole answer here, as
    // it is for the two indirections.
    case TYPE_STRING:
        return true;

    // Characters nothing holds. A 'ref str' names them and owns nothing; the
    // type itself is never a value, so it is never a value that owns.
    case TYPE_STR:
        return false;

    default:
        break;
    }

    // A struct is not itself an owner: it owns through whichever fields do, and
    // is freed field by field rather than as one value.
    for (size_t i = 0; i < type->record.field_count; i++) {
        if (type_is_owned(type->record.fields[i].type)) {
            return true;
        }
    }

    return false;
}

// Whether a value of this type can be duplicated by copying its bytes. An
// owning value cannot: two slots holding it would both free it. Anything
// reaching one transitively inherits that, so a struct is copyable exactly
// when every field is.
//
// Derived from the type rather than declared on it, so a struct becomes
// non-copyable the moment it is given a field that owns, and no declaration
// can disagree with what the type holds.
bool type_owns_a_block(const Type *type) {
    return type && type->decl && type->decl->generic && type->decl->generic->counts_a_block;
}

bool type_is_copyable(const Type *type) {
    if (!type) {
        return true;
    }

    switch (type->kind) {
    // Copying an owning pointer would make a second owner of memory only one of
    // them may free. A borrow and a raw address each carried no ownership to
    // duplicate.
    case TYPE_BOX:
        return false;
    case TYPE_REF:
    case TYPE_PTR:
        return true;

    // Copying one would make a second value freeing the same block, which is
    // what an owning pointer is refused for.
    case TYPE_BLOCK:
        return false;

    // An array copies exactly when its elements do: copying one duplicates each
    // of them, so a run of a non-copyable element is as uncopyable as one is.
    case TYPE_ARRAY:
        return type_is_copyable(type_array_element(type));

    // A string frees its characters, so copying one would make a second header
    // that frees them too.
    case TYPE_STRING:
        return false;

    // Never held, so never copied. What names characters is a reference, and
    // copying one of those duplicates no ownership.
    case TYPE_STR:
        return true;

    default:
        break;
    }

    for (size_t i = 0; i < type->record.field_count; i++) {
        if (!type_is_copyable(type->record.fields[i].type)) {
            return false;
        }
    }

    return true;
}

static TypeExpr *type_expr_create(TypeExprKind kind) {
    TypeExpr *expr = calloc(1, sizeof(TypeExpr));
    expr->kind = kind;

    return expr;
}

TypeExpr *type_expr_name(StringRef name) {
    TypeExpr *expr = type_expr_create(TYPE_EXPR_NAME);
    expr->name = name;

    return expr;
}

TypeExpr *type_expr_indirect(TypeExprKind kind, TypeExpr *inner) {
    TypeExpr *expr = type_expr_create(kind);
    expr->indirect.inner = inner;

    return expr;
}

TypeExpr *type_expr_apply(TypeExpr *base) {
    TypeExpr *expr = type_expr_create(TYPE_EXPR_APPLY);
    expr->apply.base = base;
    expr->apply.args = type_expr_list_create();

    return expr;
}

void type_expr_destroy(TypeExpr *expr) {
    if (!expr) {
        return;
    }

    switch (expr->kind) {
    case TYPE_EXPR_BOX:
    case TYPE_EXPR_REF:
        type_expr_destroy(expr->indirect.inner);
        break;
    case TYPE_EXPR_APPLY:
        type_expr_destroy(expr->apply.base);
        type_expr_list_free(&expr->apply.args);
        break;
    case TYPE_EXPR_NAME:
        break;
    }

    free(expr);
}

// Mixed rather than summed, so that '[int; 3]' and '[int; 4]' -- and an
// application whose arguments were given in the other order -- land in
// different buckets. dj2b's shift-and-add over each argument's bytes, which is
// what the string hash beside it does over characters.
size_t type_app_hash_of(TypeApp app) {
    size_t hash = 5381;

    hash = ((hash << 5) + hash) + (size_t)app.ctor;
    hash = ((hash << 5) + hash) + (size_t)(uintptr_t)app.decl;

    for (size_t i = 0; i < app.arg_count; i++) {
        const TypeArg *arg = &app.args[i];

        hash = ((hash << 5) + hash) + (size_t)arg->kind;

        // A type argument is compared by identity, so its address is what
        // distinguishes it; a const argument is its value.
        hash = ((hash << 5) + hash) +
               (arg->kind == TYPE_ARG_TYPE ? (size_t)(uintptr_t)arg->type : (size_t)(uint32_t)arg->value);
    }

    return hash;
}

bool type_app_equals(TypeApp app, TypeApp other) {
    if (app.ctor != other.ctor || app.decl != other.decl || app.arg_count != other.arg_count) {
        return false;
    }

    for (size_t i = 0; i < app.arg_count; i++) {
        if (app.args[i].kind != other.args[i].kind) {
            return false;
        }

        // An interned type is one Type, so identity is the whole comparison.
        if (app.args[i].kind == TYPE_ARG_TYPE) {
            if (app.args[i].type != other.args[i].type) {
                return false;
            }
        } else if (app.args[i].value != other.args[i].value) {
            return false;
        }
    }

    return true;
}
