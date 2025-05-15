#include "ast.h"
#include <stdlib.h>

ASTExpr *ast_expr_create() { return malloc(sizeof(ASTExpr)); }

ASTExpr *ast_literal_expr_create(Variant value) {
    ASTExpr *node = ast_expr_create();
    node->type = EXPR_LITERAL;
    node->literal.value = value;
    return node;
}

ASTExpr *ast_bin_op_expr_create(ASTExpr *left, BinOp op, ASTExpr *right) {
    ASTExpr *node = ast_expr_create();
    node->type = EXPR_BIN_OP;
    node->bin_op.left = left;
    node->bin_op.right = right;
    node->bin_op.op = op;
    return node;
}

ASTExpr *ast_variable_expr_create(char *name) {
    ASTExpr *node = ast_expr_create();
    node->type = EXPR_VARIABLE;
    node->variable.name = name;
    return node;
}

ASTStmt *ast_stmt_create() { return malloc(sizeof(ASTStmt)); }

ASTStmt *ast_expr_stmt_create(ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->type = STMT_EXPR;
    stmt->expr.value = value;
    return stmt;
}

ASTStmt *ast_var_decl_stmt_create(char *name, ASTExpr *initializer) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->type = STMT_VAR_DECL;
    stmt->var_decl.name = name;
    stmt->var_decl.initializer = initializer;
    return stmt;
}

ASTStmt *ast_assign_stmt_create(ASTExpr *target, ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->type = STMT_ASSIGN;
    stmt->assign.target = target;
    stmt->assign.value = value;
    return stmt;
}

void ast_expr_free(ASTExpr *expr) {
    if (!expr)
        return;

    switch (expr->type) {
    case EXPR_BIN_OP:
        ast_expr_free(expr->bin_op.left);
        ast_expr_free(expr->bin_op.right);
        break;
        break;
    case EXPR_VARIABLE:
        free(expr->variable.name);
        break;
    case EXPR_LITERAL:
        break;
    }

    free(expr);
}

void ast_stmt_free(ASTStmt *stmt) {
    if (!stmt)
        return;

    switch (stmt->type) {
    case STMT_EXPR:
        ast_expr_free(stmt->expr.value);
        break;
    case STMT_VAR_DECL:
        ast_expr_free(stmt->var_decl.initializer);
        free(stmt->var_decl.name);
        break;
    case STMT_ASSIGN:
        ast_expr_free(stmt->assign.target);
        ast_expr_free(stmt->assign.value);
        break;
    }

    free(stmt);
}

ASTScript *ast_script_create() {
    ASTScript *script = malloc(sizeof(ASTScript));
    script->statements = NULL;
    script->count = 0;
    return script;
}

void ast_script_add_statement(ASTScript *script, ASTStmt *stmt) {
    script->count++;
    script->statements = realloc(script->statements, script->count * sizeof(ASTStmt *));
    script->statements[script->count - 1] = stmt;
}

void ast_script_free(ASTScript *script) {
    if (!script)
        return;

    for (size_t i = 0; i < script->count; i++) {
        ast_stmt_free(script->statements[i]);
    }

    free(script->statements);
    free(script);
}
