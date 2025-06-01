#include "stmt.h"

ASTStmt *ast_stmt_create() { return malloc(sizeof(ASTStmt)); }

ASTStmt *ast_expr_stmt_create(ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->kind = STMT_EXPR;
    stmt->expr.value = value;
    return stmt;
}

ASTStmt *ast_var_decl_stmt_create(StringRef name, TypeSpec *type_spec, ASTExpr *initializer) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->kind = STMT_VAR_DECL;
    stmt->var_decl.name = name;
    stmt->var_decl.type_spec = type_spec;
    stmt->var_decl.initializer = initializer;
    stmt->var_decl.symbol = NULL;
    return stmt;
}

ASTStmt *ast_func_decl_stmt_create(StringRef name, TypeSpec *return_type, ASTFieldList params,
                                   ASTStmt *body) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->kind = STMT_FUNC_DECL;
    stmt->func_decl.name = name;
    stmt->func_decl.return_type = return_type;
    stmt->func_decl.params = params;
    stmt->func_decl.body = body;
    stmt->func_decl.symbol = NULL;
    return stmt;
}

ASTStmt *ast_assign_stmt_create(ASTExpr *target, ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->kind = STMT_ASSIGN;
    stmt->assign.target = target;
    stmt->assign.value = value;
    return stmt;
}

ASTStmt *ast_if_stmt_create(ASTExpr *condition, ASTStmt *then_block, ASTStmt *else_block) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->kind = STMT_IF;
    stmt->ifstmt.condition = condition;
    stmt->ifstmt.then_block = then_block;
    stmt->ifstmt.else_block = else_block;
    return stmt;
}

ASTStmt *ast_block_stmt_create(ASTStmtList list) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->kind = STMT_BLOCK;
    stmt->block.list = list;
    return stmt;
}

ASTStmt *ast_return_stmt_create(ASTExpr *result) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->kind = STMT_RETURN;
    stmt->ret.result = result;
    return stmt;
}

void ast_stmt_destroy(ASTStmt *stmt) {
    if (!stmt)
        return;

    switch (stmt->kind) {
    case STMT_EXPR:
        break;
    case STMT_VAR_DECL:
        if (stmt->var_decl.type_spec) {
            type_spec_destroy(stmt->var_decl.type_spec);
        }
        break;
    case STMT_FUNC_DECL:
        if (stmt->func_decl.return_type) {
            type_spec_destroy(stmt->func_decl.return_type);
        }
        ast_field_list_free(&stmt->func_decl.params);
        ast_stmt_destroy(stmt->func_decl.body);
        break;
    case STMT_ASSIGN:
        break;
    case STMT_IF:
        ast_stmt_destroy(stmt->ifstmt.then_block);
        ast_stmt_destroy(stmt->ifstmt.else_block);
        break;
    case STMT_BLOCK:
        ast_stmt_list_free(&stmt->block.list);
        break;
    case STMT_RETURN:
        break;
    }

    free(stmt);
}

ASTField *ast_field_create(StringRef name, TypeSpec *type_spec) {
    ASTField *field = malloc(sizeof(ASTField));
    field->name = name;
    field->type_spec = type_spec;
    field->symbol = NULL;

    return field;
}

void ast_field_destroy(ASTField *field) {
    type_spec_destroy(field->type_spec);
    free(field);
}
