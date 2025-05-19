#include "ast.h"
#include "string_ref.h"
#include "symbol_table.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

ASTExpr *ast_variable_expr_create(StringRef name) {
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

ASTStmt *ast_var_decl_stmt_create(StringRef name, ASTExpr *initializer) {
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

ASTStmt *ast_return_stmt_create(ASTExpr *result) {
    ASTStmt *stmt = ast_stmt_create();
    stmt->type = STMT_RETURN;
    stmt->ret.result = result;
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
    default:
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
        break;
    case STMT_ASSIGN:
        ast_expr_free(stmt->assign.target);
        ast_expr_free(stmt->assign.value);
        break;
    case STMT_RETURN:
        ast_expr_free(stmt->ret.result);
        break;
    }

    free(stmt);
}

ASTScript *ast_script_create() {
    ASTScript *script = malloc(sizeof(ASTScript));
    script->statements = NULL;
    script->statements_size = 0;
    script->vars_count = 0;
    script->symbol_table = symbol_table_create(SYMBOL_TABLE_INITIAL_CAPACITY);
    return script;
}

void ast_script_add_statement(ASTScript *script, ASTStmt *stmt) {
    script->statements_size++;
    script->statements = realloc(script->statements, script->statements_size * sizeof(ASTStmt *));
    script->statements[script->statements_size - 1] = stmt;
}

void ast_script_expr_visit(ASTScript *script, ASTExpr *expr) {
    switch (expr->type) {
    case EXPR_BIN_OP: {
        ast_script_expr_visit(script, expr->bin_op.left);
        ast_script_expr_visit(script, expr->bin_op.right);
        break;
    }
    case EXPR_VARIABLE: {
        char *key = string_ref_to_cstr(expr->variable.name);
        SymbolEntry *entry = symbol_table_lookup(script->symbol_table, key);
        free(key);

        assert(entry && "undeclared variable");

        expr->symbol = entry->symbol;
    }
    default:
        break;
    }
}

void ast_script_stmt_visit(ASTScript *script, ASTStmt *stmt) {
    switch (stmt->type) {
    case STMT_EXPR: {
        ast_script_expr_visit(script, stmt->expr.value);
        break;
    }
    case STMT_VAR_DECL: {
        char *key = string_ref_to_cstr(stmt->var_decl.name);
        bool ok = symbol_table_insert(script->symbol_table, key, script->vars_count);
        free(key);

        assert(ok && "variable already declared");

        if (stmt->var_decl.initializer) {
            ast_script_expr_visit(script, stmt->var_decl.initializer);
        }

        stmt->var_decl.reg = script->vars_count++;
        break;
    }
    case STMT_ASSIGN: {
        ast_script_expr_visit(script, stmt->assign.target);
        ast_script_expr_visit(script, stmt->assign.value);
        break;
    }
    case STMT_RETURN: {
        ast_script_expr_visit(script, stmt->ret.result);
        break;
    }
    }
}

void ast_script_resolve_symbols(ASTScript *script) {
    for (int i = 0; i < script->statements_size; i++) {
        ast_script_stmt_visit(script, script->statements[i]);
    }
}

void ast_script_free(ASTScript *script) {
    if (!script)
        return;

    symbol_table_free(script->symbol_table);

    for (size_t i = 0; i < script->statements_size; i++) {
        ast_stmt_free(script->statements[i]);
    }

    free(script->statements);
    free(script);
}
