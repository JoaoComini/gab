#include "ast.h"

#include "string/string.h"
#include "string/string_ref.h"
#include "symbol_table.h"
#include "type.h"
#include "type_registry.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

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

void ast_stmt_free(ASTStmt *stmt) {
    if (!stmt)
        return;

    switch (stmt->kind) {
    case STMT_EXPR:
        ast_expr_free(stmt->expr.value);
        break;
    case STMT_VAR_DECL:
        ast_expr_free(stmt->var_decl.initializer);
        if (stmt->var_decl.type_spec) {
            type_spec_destroy(stmt->var_decl.type_spec);
        }
        break;
    case STMT_ASSIGN:
        ast_expr_free(stmt->assign.target);
        ast_expr_free(stmt->assign.value);
        break;
    case STMT_IF:
        ast_expr_free(stmt->ifstmt.condition);
        ast_stmt_free(stmt->ifstmt.then_block);
        ast_stmt_free(stmt->ifstmt.else_block);
        break;
    case STMT_BLOCK:
        ast_stmt_list_free(stmt->block.list);
        break;
    case STMT_RETURN:
        ast_expr_free(stmt->ret.result);
        break;
    }

    free(stmt);
}

ASTStmtList ast_stmt_list_create() {
    return (ASTStmtList){
        .data = NULL,
        .size = 0,
        .capacity = 0,
    };
}

void ast_stmt_list_add(ASTStmtList *list, ASTStmt *stmt) {
    if (list->size >= list->capacity) {
        list->capacity = list->capacity == 0 ? 1 : list->capacity * 2;
        list->data = realloc(list->data, list->capacity * sizeof(ASTStmt *));
    }

    list->data[list->size++] = stmt;
}

void ast_stmt_list_free(ASTStmtList list) {
    for (int i = 0; i < list.size; i++) {
        ast_stmt_free(list.data[i]);
    }
    free(list.data);
}

ASTScript *ast_script_create() {
    ASTScript *script = malloc(sizeof(ASTScript));
    script->statements = ast_stmt_list_create();
    script->vars_count = 0;
    script->global_scope = scope_create(NULL);
    script->type_registry = type_registry_create();

    return script;
}

void ast_script_destroy(ASTScript *script) {
    if (!script)
        return;

    scope_destroy(script->global_scope);
    type_registry_destroy(script->type_registry);
    ast_stmt_list_free(script->statements);

    free(script);
}

void ast_script_add_statement(ASTScript *script, ASTStmt *stmt) {
    ast_stmt_list_add(&script->statements, stmt);
}

bool is_numeric_type(Type *t) { return t->kind == TYPE_INT || t->kind == TYPE_FLOAT; }

bool is_boolean_type(Type *t) { return t->kind == TYPE_BOOL; }

bool is_ordered_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

bool is_comparable_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

void ast_script_expr_visit(ASTScript *script, ASTExpr *expr, Scope *scope) {
    if (!expr) {
        return;
    }

    switch (expr->kind) {
    case EXPR_BIN_OP: {
        ast_script_expr_visit(script, expr->bin_op.left, scope);
        ast_script_expr_visit(script, expr->bin_op.right, scope);

        Type *left_type = expr->bin_op.left->type;
        Type *right_type = expr->bin_op.right->type;

        assert(left_type == right_type && "mismatched types in binary operation");

        // Validate operator compatibility with type
        switch (expr->bin_op.op) {
        case BIN_OP_ADD:
        case BIN_OP_SUB:
        case BIN_OP_MUL:
        case BIN_OP_DIV:
            assert(is_numeric_type(left_type) && "arithmetic ops only valid on numeric types");
            expr->type = left_type;
            return;
        case BIN_OP_EQUAL:
        case BIN_OP_NEQUAL:
            assert(is_comparable_type(left_type) && "equality not supported for this type");
            break;
        case BIN_OP_LESS:
        case BIN_OP_GREATER:
        case BIN_OP_LEQUAL:
        case BIN_OP_GEQUAL:
            assert(is_ordered_type(left_type) && "relational ops only valid on ordered types");
            break;
        case BIN_OP_AND:
        case BIN_OP_OR:
            assert(is_boolean_type(left_type) && "logical ops only valid on boolean types");
            break;
        }

        expr->type = type_registry_get_builtin(script->type_registry, TYPE_BOOL);
        break;
    }
    case EXPR_VARIABLE: {
        Symbol *entry = scope_symbol_lookup(scope, string_from_ref(expr->var.name));
        assert(entry && "undeclared variable");

        expr->symbol = *entry;
        expr->type = entry->type;
        break;
    }
    case EXPR_LITERAL: {
        expr->type = type_registry_get_builtin(script->type_registry, expr->lit.kind);
        break;
    }
    default:
        break;
    }
}

void ast_script_stmt_visit(ASTScript *script, ASTStmt *stmt, Scope *scope) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
    case STMT_EXPR: {
        ast_script_expr_visit(script, stmt->expr.value, scope);
        break;
    }
    case STMT_VAR_DECL: {
        ast_script_expr_visit(script, stmt->var_decl.initializer, scope);

        Type *type;
        if (stmt->var_decl.type_spec) {
            Type *decl_type =
                type_registry_get(script->type_registry, string_from_ref(stmt->var_decl.type_spec->name));
            assert(decl_type && "unknown type");

            if (stmt->var_decl.initializer) {
                Type *init_type = stmt->var_decl.initializer->type;

                assert(decl_type == init_type && "mismatched types in assignment");
            }

            type = decl_type;
        } else {
            type = stmt->var_decl.initializer->type;
        }

        Symbol *var = scope_decl_var(scope, string_from_ref(stmt->var_decl.name), type);

        assert(var && "variable already declared in this scope");

        stmt->var_decl.symbol = *var;
        script->vars_count++;
        break;
    }
    case STMT_ASSIGN: {
        ast_script_expr_visit(script, stmt->assign.target, scope);
        ast_script_expr_visit(script, stmt->assign.value, scope);

        Type *target_type = stmt->assign.target->type;
        Type *value_type = stmt->assign.value->type;

        assert(target_type == value_type && "mismatched types in assignment");
        break;
    }
    case STMT_IF: {
        ast_script_expr_visit(script, stmt->ifstmt.condition, scope);
        ast_script_stmt_visit(script, stmt->ifstmt.then_block, scope);
        ast_script_stmt_visit(script, stmt->ifstmt.else_block, scope);
        break;
    }
    case STMT_BLOCK: {
        Scope block_scope;
        scope_init(&block_scope, scope);

        for (int i = 0; i < stmt->block.list.size; i++) {
            ast_script_stmt_visit(script, stmt->block.list.data[i], &block_scope);
        }

        if (block_scope.var_offset > script->vars_count) {
            script->vars_count = block_scope.var_offset;
        }

        scope_free(&block_scope);
        break;
    }
    case STMT_RETURN: {
        ast_script_expr_visit(script, stmt->ret.result, scope);
        break;
    }
    }
}

void ast_script_resolve(ASTScript *script) {
    for (int i = 0; i < script->statements.size; i++) {
        ast_script_stmt_visit(script, script->statements.data[i], script->global_scope);
    }
}
