#include "ast/type_expr.h"

#include <string.h>

static TypeExpr *type_expr_create(Arena *arena, TypeExprKind kind) {
    TypeExpr *expr = arena_alloc(arena, sizeof(TypeExpr));
    memset(expr, 0, sizeof(TypeExpr));
    expr->kind = kind;

    return expr;
}

TypeExpr *type_expr_name(Arena *arena, StringRef name) {
    TypeExpr *expr = type_expr_create(arena, TYPE_EXPR_NAME);
    expr->name = name;

    return expr;
}

TypeExpr *type_expr_indirect(Arena *arena, TypeExprKind kind, TypeExpr *inner) {
    TypeExpr *expr = type_expr_create(arena, kind);
    expr->indirect.inner = inner;

    return expr;
}

TypeExpr *type_expr_apply(Arena *arena, TypeExpr *base) {
    TypeExpr *expr = type_expr_create(arena, TYPE_EXPR_APPLY);
    expr->apply.base = base;
    expr->apply.args = type_expr_list_create(arena_allocator(arena));

    return expr;
}

TypeExpr *type_expr_array(Arena *arena, TypeExpr *element, int32_t length) {
    TypeExpr *expr = type_expr_create(arena, TYPE_EXPR_ARRAY);
    expr->array.element = element;
    expr->array.length = length;

    return expr;
}
