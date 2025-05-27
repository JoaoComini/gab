#include "expr.h"

ASTExpr *ast_expr_create() { return malloc(sizeof(ASTExpr)); }

ASTExpr *ast_literal_expr_create(Literal value) {
    ASTExpr *node = ast_expr_create();
    node->kind = EXPR_LITERAL;
    node->lit = value;
    return node;
}

ASTExpr *ast_bin_op_expr_create(ASTExpr *left, BinOp op, ASTExpr *right) {
    ASTExpr *node = ast_expr_create();
    node->kind = EXPR_BIN_OP;
    node->bin_op.left = left;
    node->bin_op.right = right;
    node->bin_op.op = op;
    return node;
}

ASTExpr *ast_variable_expr_create(StringRef name) {
    ASTExpr *node = ast_expr_create();
    node->kind = EXPR_VARIABLE;
    node->var.name = name;
    return node;
}

void ast_expr_free(ASTExpr *expr) {
    if (!expr)
        return;

    switch (expr->kind) {
    case EXPR_BIN_OP:
        ast_expr_free(expr->bin_op.left);
        ast_expr_free(expr->bin_op.right);
        break;
    default:
        break;
    }

    free(expr);
}
