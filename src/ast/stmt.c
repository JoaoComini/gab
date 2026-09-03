#include "stmt.h"

ASTStmt *ast_stmt_create(Arena *arena, Span span) {
    ASTStmt *stmt = arena_alloc(arena, sizeof(ASTStmt));
    stmt->span = span;

    return stmt;
}

ASTStmt *ast_expr_stmt_create(Arena *arena, Span span, ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_EXPR;
    stmt->expr.value = value;
    return stmt;
}

ASTStmt *ast_var_decl_stmt_create(Arena *arena, Span span, StringRef name, TypeExpr *type_expr,
                                  ASTExpr *initializer) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_VAR_DECL;
    stmt->var_decl.name = name;
    stmt->var_decl.type_expr = type_expr;
    stmt->var_decl.initializer = initializer;
    stmt->var_decl.binding = NULL;
    return stmt;
}

ASTStmt *ast_func_decl_stmt_create(Arena *arena, Span span, StringRef name, TypeExpr *return_type,
                                   ASTFieldList params, ASTStmt *body) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_FUNC_DECL;
    stmt->func_decl.name = name;
    stmt->func_decl.owner = NULL;
    stmt->func_decl.return_type = return_type;
    stmt->func_decl.params = params;
    stmt->func_decl.body = body;
    stmt->func_decl.type_param_count = 0;
    stmt->func_decl.function = NULL;
    stmt->func_decl.resolved_return_type = NULL;
    stmt->func_decl.declared = false;
    return stmt;
}

ASTStmt *ast_struct_decl_stmt_create(Arena *arena, Span span, StringRef name, const StringRef *params,
                                     size_t param_count, ASTFieldList fields) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_STRUCT_DECL;
    stmt->struct_decl.name = name;
    stmt->struct_decl.fields = fields;
    stmt->struct_decl.param_count = param_count;

    for (size_t i = 0; i < param_count; i++) {
        stmt->struct_decl.params[i] = params[i];
    }

    stmt->struct_decl.declared = false;
    return stmt;
}

ASTStmt *ast_impl_stmt_create(Arena *arena, Span span, TypeExpr *type, ASTStmtList members) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_IMPL;
    stmt->impl.type = type;
    stmt->impl.members = members;
    return stmt;
}

ASTStmt *ast_interface_decl_stmt_create(Arena *arena, Span span, StringRef name, ASTStmtList members) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_INTERFACE_DECL;
    stmt->interface_decl.name = name;
    stmt->interface_decl.members = members;
    return stmt;
}

ASTStmt *ast_assign_stmt_create(Arena *arena, Span span, ASTExpr *target, ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_ASSIGN;
    stmt->assign.target = target;
    stmt->assign.value = value;
    return stmt;
}

ASTStmt *ast_compound_assign_stmt_create(Arena *arena, Span span, ASTExpr *target, BinOp op, ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_COMPOUND_ASSIGN;
    stmt->compound_assign.target = target;
    stmt->compound_assign.value = value;
    stmt->compound_assign.op = op;
    return stmt;
}

ASTStmt *ast_if_stmt_create(Arena *arena, Span span, ASTExpr *condition, ASTStmt *then_block,
                            ASTStmt *else_block) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_IF;
    stmt->ifstmt.condition = condition;
    stmt->ifstmt.then_block = then_block;
    stmt->ifstmt.else_block = else_block;
    return stmt;
}

ASTStmt *ast_for_stmt_create(Arena *arena, Span span, ASTStmt *init, ASTExpr *condition, ASTStmt *post,
                             ASTStmt *body) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_FOR;
    stmt->forstmt.init = init;
    stmt->forstmt.condition = condition;
    stmt->forstmt.post = post;
    stmt->forstmt.body = body;
    stmt->forstmt.scope = NULL;
    return stmt;
}

ASTStmt *ast_jump_stmt_create(Arena *arena, Span span, bool is_break) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_JUMP;
    stmt->jump.is_break = is_break;
    return stmt;
}

ASTStmt *ast_block_stmt_create(Arena *arena, Span span, ASTStmtList list) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_BLOCK;
    stmt->block.list = list;
    stmt->block.scope = NULL;
    return stmt;
}

ASTStmt *ast_return_stmt_create(Arena *arena, Span span, ASTExpr *result) {
    ASTStmt *stmt = ast_stmt_create(arena, span);
    stmt->kind = STMT_RETURN;
    stmt->ret.result = result;
    return stmt;
}

ASTField *ast_field_create(Arena *arena, Span span, StringRef name, TypeExpr *type_expr) {
    ASTField *field = arena_alloc(arena, sizeof(ASTField));
    field->span = span;
    field->name = name;
    field->type_expr = type_expr;
    field->binding = NULL;

    return field;
}
