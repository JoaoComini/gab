#include "expr.h"

ASTExpr *ast_expr_create(Arena *arena, Span span) {
    ASTExpr *node = arena_alloc(arena, sizeof(ASTExpr));
    node->span = span;

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

ASTIdent *ast_ident_create(Arena *arena, StringPool *strings, Span span, StringRef name) {
    ASTIdent *ident = arena_alloc(arena, sizeof(ASTIdent));
    ident->name = string_from_ref(strings, name);
    ident->span = span;

    return ident;
}

ASTExpr *ast_name_expr_create(Arena *arena, Span span, ASTIdent *name) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_NAME;
    node->name.name = name;
    node->name.owner_type_expr = NULL;
    return node;
}

ASTExpr *ast_qualified_expr_create(Arena *arena, Span span, ASTIdent *qualifier, ASTIdent *name,
                                   TypeExpr *owner_type_expr) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_QUALIFIED;
    node->qualified.qualifier = qualifier;
    node->qualified.name = name;
    node->qualified.owner_type_expr = owner_type_expr;
    return node;
}

ASTExpr *ast_builtin_expr_create(Arena *arena, Span span, ASTIdent *name, TypeExpr *type_expr) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_BUILTIN;
    node->builtin.name = name;
    node->builtin.type_expr = type_expr;
    return node;
}

ASTExpr *ast_call_expr_create(Arena *arena, Span span, ASTExpr *target, ASTExprList args) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_CALL;
    node->call.target = target;
    node->call.args = args;
    return node;
}

ASTExpr *ast_field_expr_create(Arena *arena, Span span, ASTExpr *target, ASTIdent *name) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_FIELD;
    node->field.target = target;
    node->field.name = name;
    return node;
}

ASTExpr *ast_addr_of_expr_create(Arena *arena, Span span, ASTExpr *target) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_ADDR_OF;
    node->unary.target = target;
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

ASTExpr *ast_box_expr_create(Arena *arena, Span span, ASTExpr *value) {
    ASTExpr *node = ast_expr_create(arena, span);
    node->kind = EXPR_BOX;
    node->box_expr.value = value;
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

TypeKind literal_type_kind(LiteralKind kind) {
    switch (kind) {
    case LITERAL_FLOAT:
        return TYPE_F32;
    case LITERAL_BOOL:
        return TYPE_BOOL;
    case LITERAL_STRING:
        return TYPE_STR;
    case LITERAL_INT:
        break;
    }

    return TYPE_I32;
}
