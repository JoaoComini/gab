#include "type.h"

#include "util/align.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

_Static_assert(sizeof(int32_t) == 4, "gab int must be 4 bytes");
_Static_assert(sizeof(float) == 4, "gab float must be 4 bytes");

Type *type_create(Arena *arena, TypeKind kind, String *name) {
    Type *type = arena_alloc(arena, sizeof(Type));
    type->kind = kind;
    type->name = name;
    type->size = 0;
    type->alignment = 1;
    type->fields = NULL;
    type->field_count = 0;
    type->inner = NULL;
    type->element = NULL;
    type->methods = NULL;
    type->owner = NULL;
    type->drop = NULL;

    return type;
}

Type *type_struct_create(Arena *arena, String *name, size_t max_fields) {
    Type *type = type_create(arena, TYPE_STRUCT, name);

    if (max_fields > 0) {
        type->fields = arena_alloc(arena, max_fields * sizeof(TypeField));
    }

    return type;
}

void type_add_field(Type *type, String *name, Type *field_type) {
    assert(type->fields && "struct was created without room for fields");

    type->fields[type->field_count++] = (TypeField){
        .name = name,
        .type = field_type,
        .offset = 0,
    };
}

void type_layout_compute(Type *type) {
    size_t offset = 0;
    size_t alignment = 1;

    for (size_t i = 0; i < type->field_count; i++) {
        TypeField *field = &type->fields[i];

        offset = align_up(offset, field->type->alignment);
        field->offset = offset;
        offset += field->type->size;

        if (field->type->alignment > alignment) {
            alignment = field->type->alignment;
        }
    }

    type->alignment = alignment;
    type->size = align_up(offset, alignment);
}

const TypeField *type_find_field(const Type *type, const String *name) {
    for (size_t i = 0; i < type->field_count; i++) {
        const TypeField *field = &type->fields[i];

        if (field->name == name) {
            return field;
        }
    }

    return NULL;
}

bool type_field_offset(const Type *type, const String *name, size_t *out_offset) {
    const TypeField *field = type_find_field(type, name);

    if (!field) {
        return false;
    }

    *out_offset = field->offset;
    return true;
}

// A raw pointer is deliberately not one of these. Every caller is a
// language-level path -- an auto-deref, a lend, a field access -- and a 'ptr T'
// is reachable from none of them: it names a block the header beside it
// describes, and reaching through it is that header's business.
bool type_is_indirect(const Type *type) { return type && (type->kind == TYPE_BOX || type->kind == TYPE_REF); }

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

    // A raw address claims nothing about what it names, so nothing frees
    // through one. The header naming a block is what owns it.
    case TYPE_PTR:
        return false;

    // A header owns exactly what it was built to free. The raw address naming
    // its block says nothing about it, and its fields say nothing either, so
    // the drop chosen where the type was built is the whole answer.
    case TYPE_STRING:
    case TYPE_ARRAY:
        return type->drop != NULL;

    default:
        break;
    }

    // A struct is not itself an owner: it owns through whichever fields do, and
    // is freed field by field rather than as one value.
    for (size_t i = 0; i < type->field_count; i++) {
        if (type_is_owned(type->fields[i].type)) {
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

    // A header that frees its block cannot be copied, for the same reason.
    case TYPE_STRING:
    case TYPE_ARRAY:
        return type->drop == NULL;

    default:
        break;
    }

    for (size_t i = 0; i < type->field_count; i++) {
        if (!type_is_copyable(type->fields[i].type)) {
            return false;
        }
    }

    return true;
}

// Sized for a handful: most struct types declare no methods at all, and the map
// grows if one proves popular.
#define METHOD_MAP_INITIAL_CAPACITY 4

bool type_add_method(Arena *arena, Type *type, String *name, Symbol *method) {
    if (!type->methods) {
        type->methods = method_map_create_alloc(arena_allocator(arena), METHOD_MAP_INITIAL_CAPACITY);
    }

    if (method_map_lookup(type->methods, name)) {
        return false;
    }

    method_map_insert(type->methods, name, method);
    return true;
}

Symbol *type_find_method(const Type *type, const String *name) {
    if (!type) {
        return NULL;
    }

    // A borrow reads the method set of what it borrows, since a method that
    // only reads its receiver is meaningful on both. Its own set is consulted
    // first: what the two do not share is what tells them apart, and a method
    // declared on the borrow answers for the borrow alone.
    if (type->methods) {
        Symbol **found = method_map_lookup(type->methods, (String *)name);

        if (found) {
            return *found;
        }
    }

    // Only a borrow follows this, and finding a method here is not yet a call
    // that resolves: the owner's methods declare an owning receiver, which a
    // borrow does not satisfy. So 'clone', declared on the owning string, is
    // found from a borrow and then refused where the receiver is reconciled.
    if (type->owner) {
        return type_find_method(type->owner, name);
    }

    return NULL;
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
