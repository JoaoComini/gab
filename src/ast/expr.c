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

ASTExpr *ast_field_expr_create(Span span, ASTExpr *target, StringRef name) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_FIELD;
    node->field.target = target;
    node->field.name = name;
    node->field.owner = NULL;
    node->field.index = 0;
    return node;
}

ASTExpr *ast_addr_of_expr_create(Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_ADDR_OF;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_move_expr_create(Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_MOVE;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_deref_expr_create(Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_DEREF;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_neg_expr_create(Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_NEG;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_not_expr_create(Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_NOT;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_cast_expr_create(Span span, ASTExpr *operand) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_CAST;
    node->cast.operand = operand;
    return node;
}

ASTExpr *ast_new_expr_create(Span span, TypeExpr *type_expr) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_NEW;
    node->new_expr.type_expr = type_expr;
    node->new_expr.type = NULL;
    return node;
}

ASTExpr *ast_array_lit_expr_create(Span span, ASTExprList elements) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_ARRAY_LIT;
    node->array_lit.elements = elements;
    return node;
}

ASTExpr *ast_index_expr_create(Span span, ASTExpr *target, ASTExpr *index) {
    ASTExpr *node = ast_expr_create(span);
    node->kind = EXPR_INDEX;
    node->index.target = target;
    node->index.index = index;
    node->index.array_type = NULL;
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
    case EXPR_FIELD:
        ast_expr_free(expr->field.target);
        break;
    case EXPR_ADDR_OF:
    case EXPR_DEREF:
    case EXPR_NEG:
    case EXPR_NOT:
    case EXPR_MOVE:
        ast_expr_free(expr->unary.target);
        break;
    case EXPR_CAST:
        ast_expr_free(expr->cast.operand);
        break;
    case EXPR_NEW:
        type_expr_destroy(expr->new_expr.type_expr);
        break;
    case EXPR_ARRAY_LIT:
        for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
            ast_expr_free(expr->array_lit.elements.data[i]);
        }
        ast_expr_list_free(&expr->array_lit.elements);
        break;
    case EXPR_INDEX:
        ast_expr_free(expr->index.target);
        ast_expr_free(expr->index.index);
        break;
        break;
    default:
        break;
    }

    free(expr);
}

const TypeField *ast_field_of(const ASTExpr *expr) {
    if (!expr->field.owner) {
        return NULL;
    }

    return &type_fields(expr->field.owner)[expr->field.index];
}
