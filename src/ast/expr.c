#include "expr.h"

ASTExpr *ast_expr_create(Arena *arena, Span span) {
    ASTExpr *node = arena_alloc(arena, sizeof(ASTExpr));
    node->span = span;
    node->type = NULL;
    node->binding = NULL;
    node->callee = NULL;
    node->moves = false;

    return node;
}

ASTExpr *ast_literal_expr_create(Arena *arena, Span span, Literal value) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_LITERAL;
    node->lit = value;
    return node;
}

ASTExpr *ast_bin_op_expr_create(Arena *arena, Span span, ASTExpr *left, BinOp op, ASTExpr *right) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_BIN_OP;
    node->bin_op.left = left;
    node->bin_op.right = right;
    node->bin_op.op = op;
    return node;
}

ASTExpr *ast_variable_expr_create(Arena *arena, Span span, StringRef name) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_VARIABLE;
    node->var.name = name;
    node->var.owner_type_expr = NULL;
    return node;
}

ASTExpr *ast_call_expr_create(Arena *arena, Span span, ASTExpr *target, ASTExprList args) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_CALL;
    node->call.target = target;
    node->call.args = args;
    return node;
}

ASTExpr *ast_field_expr_create(Arena *arena, Span span, ASTExpr *target, StringRef name) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_FIELD;
    node->field.target = target;
    node->field.name = name;
    node->field.owner = NULL;
    node->field.index = 0;
    return node;
}

ASTExpr *ast_addr_of_expr_create(Arena *arena, Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_ADDR_OF;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_lend_expr_create(Arena *arena, Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_LEND;
    node->lend.target = target;
    node->lend.parts = NULL;
    node->lend.part_count = 0;

    return node;
}

ASTExpr *ast_unsize_expr_create(Arena *arena, Span span, ASTExpr *target, int32_t length) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_UNSIZE;
    node->unsize.target = target;
    node->unsize.length = length;

    return node;
}

ASTExpr *ast_deref_expr_create(Arena *arena, Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_DEREF;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_neg_expr_create(Arena *arena, Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_NEG;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_not_expr_create(Arena *arena, Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_NOT;
    node->unary.target = target;
    return node;
}

ASTExpr *ast_cast_expr_create(Arena *arena, Span span, ASTExpr *operand) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_CAST;
    node->cast.operand = operand;
    return node;
}

ASTExpr *ast_box_expr_create(Arena *arena, Span span, ASTExpr *value) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_BOX;
    node->box_expr.value = value;
    node->box_expr.type = NULL;
    return node;
}

ASTExpr *ast_array_lit_expr_create(Arena *arena, Span span, ASTExprList elements) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_ARRAY_LIT;
    node->array_lit.elements = elements;
    return node;
}

ASTExpr *ast_struct_lit_expr_create(Arena *arena, Span span, TypeExpr *type_expr, ASTFieldInitList fields) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_STRUCT_LIT;
    node->struct_lit.type_expr = type_expr;
    node->struct_lit.fields = fields;
    return node;
}

ASTExpr *ast_index_expr_create(Arena *arena, Span span, ASTExpr *target, ASTExpr *index) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_INDEX;
    node->index.target = target;
    node->index.index = index;
    return node;
}

Binding *ast_binding_of(const ASTExpr *expr) { return expr->binding; }

void ast_bind(ASTExpr *expr, Binding *binding) { expr->binding = binding; }

Binding *ast_root_local(const ASTExpr *expr) {
    while (expr) {
        switch (expr->kind) {
        case EXPR_VARIABLE:
            return ast_binding_of(expr);
        case EXPR_FIELD:
            expr = expr->field.target;
            break;
        case EXPR_INDEX:
            expr = expr->index.target;
            break;
        case EXPR_DEREF:
        case EXPR_ADDR_OF:
            expr = expr->unary.target;
            break;
        default:
            return NULL;
        }
    }

    return NULL;
}

const TypeField *ast_field_of(TypeRegistry *registry, const ASTExpr *expr) {
    if (!expr->field.owner) {
        return NULL;
    }

    return &type_registry_fields_of(registry, expr->field.owner)->fields[expr->field.index];
}
