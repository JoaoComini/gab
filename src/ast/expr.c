#include "expr.h"
#include "arena.h"

ASTExpr *ast_expr_create(Arena *arena) { return arena_alloc(arena, sizeof(ASTExpr)); }

ASTExpr *ast_literal_expr_create(Arena *arena, Literal value) {
    ASTExpr *expr = ast_expr_create(arena);
    expr->kind = EXPR_LITERAL;
    expr->lit = value;
    return expr;
}

ASTExpr *ast_bin_op_expr_create(Arena *arena, ASTExpr *left, ASTExpr *right, BinOp op) {
    ASTExpr *expr = ast_expr_create(arena);
    expr->kind = EXPR_BIN_OP;
    expr->bin_op.left = left;
    expr->bin_op.right = right;
    expr->bin_op.op = op;
    return expr;
}

ASTExpr *ast_identifier_expr_create(Arena *arena, StringRef name) {
    ASTExpr *expr = ast_expr_create(arena);
    expr->kind = EXPR_IDENTIFIER;
    expr->ident.name = name;
    return expr;
}

ASTExpr *ast_call_expr_create(Arena *arena, ASTExpr *target, ASTExprList params) {
    ASTExpr *expr = ast_expr_create(arena);
    expr->kind = EXPR_CALL;
    expr->call.target = target;
    expr->call.params = params;

    return expr;
}
