#include "expr.h"

ASTExpr *ast_expr_create(Span span) {
    ASTExpr *node = malloc(sizeof(ASTExpr));
    node->span = span;
    node->type = NULL;
    node->symbol = NULL;

    return node;
}

ASTExpr *ast_literal_expr_create(Span span, Literal value) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_LITERAL;
    node->lit = value;
    return node;
}

ASTExpr *ast_bin_op_expr_create(Span span, ASTExpr *left, BinOp op, ASTExpr *right) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_BIN_OP;
    node->bin_op.left = left;
    node->bin_op.right = right;
    node->bin_op.op = op;
    return node;
}

ASTExpr *ast_variable_expr_create(Span span, StringRef name) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_VARIABLE;
    node->var.name = name;
    return node;
}

ASTExpr *ast_call_expr_create(Span span, ASTExpr *target, ASTExprList args) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_CALL;
    node->call.target = target;
    node->call.args = args;
    return node;
}

ASTExpr *ast_method_call_expr_create(Span span, ASTExpr *receiver, StringRef name, ASTExprList args) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_METHOD_CALL;
    node->method_call.receiver = receiver;
    node->method_call.name = name;
    node->method_call.args = args;
    node->method_call.method = NULL;
    node->method_call.take_address = false;
    node->method_call.deref = false;
    return node;
}

ASTExpr *ast_field_expr_create(Span span, ASTExpr *target, StringRef name) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_FIELD;
    node->field.target = target;
    node->field.name = name;
    node->field.field = NULL;
    return node;
}

ASTExpr *ast_addr_of_expr_create(Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_ADDR_OF;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_deref_expr_create(Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_DEREF;
    node->unary.target = target;
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
    case EXPR_CALL:
        ast_expr_free(expr->call.target);

        for (size_t i = 0; i < expr->call.args.size; i++) {
            ast_expr_free(expr->call.args.data[i]);
        }

        ast_expr_list_free(&expr->call.args);
        break;
    case EXPR_METHOD_CALL:
        ast_expr_free(expr->method_call.receiver);

        for (size_t i = 0; i < expr->method_call.args.size; i++) {
            ast_expr_free(expr->method_call.args.data[i]);
        }

        ast_expr_list_free(&expr->method_call.args);
        break;
    case EXPR_FIELD:
        ast_expr_free(expr->field.target);
        break;
    case EXPR_ADDR_OF:
    case EXPR_DEREF:
        ast_expr_free(expr->unary.target);
        break;
    default:
        break;
    }

    free(expr);
}
