#include "ast/type_expr.h"

#include <stdlib.h>

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

TypeExpr *type_expr_array(TypeExpr *element, int32_t length) {
    TypeExpr *expr = type_expr_create(TYPE_EXPR_ARRAY);
    expr->array.element = element;
    expr->array.length = length;

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
    case TYPE_EXPR_ARRAY:
        type_expr_destroy(expr->array.element);
        break;
    case TYPE_EXPR_NAME:
        break;
    }

    free(expr);
}
