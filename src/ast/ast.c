#include "ast.h"

#include "scope.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "symbol_table.h"
#include "type.h"
#include "type_registry.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

ASTScript *ast_script_create() {
    ASTScript *script = malloc(sizeof(ASTScript));
    script->statements = ast_stmt_list_create();
    script->vars_count = 0;

    return script;
}

void ast_script_destroy(ASTScript *script) {
    ast_stmt_list_free(&script->statements);

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

        expr->type = type_registry_get_builtin(scope->type_registry, TYPE_BOOL);
        break;
    }
    case EXPR_VARIABLE: {
        Symbol *entry = scope_symbol_lookup(scope, string_from_ref(expr->var.name));
        assert(entry && "undeclared variable");

        expr->symbol = *entry;
        expr->type = entry->var.type;
        break;
    }
    case EXPR_LITERAL: {
        expr->type = type_registry_get_builtin(scope->type_registry, expr->lit.kind);
        break;
    }
    default:
        break;
    }
}

Type *ast_script_resolve_type(Scope *scope, TypeSpec *spec) {
    if (!spec) {
        return NULL;
    }

    Type *type = type_registry_get(scope->type_registry, string_from_ref(spec->name));
    assert(type && "unknown type");

    return type;
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
            Type *decl_type = ast_script_resolve_type(scope, stmt->var_decl.type_spec);

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
        break;
    }
    case STMT_FUNC_DECL: {
        Scope func_scope;
        scope_init(&func_scope, SCOPE_LOCAL, NULL);

        for (int i = 0; i < stmt->func_decl.params.size; i++) {
            ASTField *param = stmt->func_decl.params.data[i];

            String *param_name = string_from_ref(param->name);
            Type *param_type = ast_script_resolve_type(scope, param->type_spec);

            Symbol *symbol = scope_decl_var(&func_scope, param_name, param_type);
            assert(symbol && "variable already declared in this scope");

            param->symbol = *symbol;
        }

        StringRef func_name = stmt->func_decl.name;
        Type *func_return_type = ast_script_resolve_type(scope, stmt->func_decl.return_type);

        ast_script_stmt_visit(script, stmt->func_decl.body, &func_scope);

        Symbol *func = scope_decl_func(scope, string_from_ref(func_name), func_return_type);
        assert(func && "function already declared in this scope");

        stmt->func_decl.symbol = *func;
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
        scope_init(&block_scope, SCOPE_LOCAL, scope);

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

void ast_script_resolve(ASTScript *script, Scope *global_scope) {
    for (int i = 0; i < script->statements.size; i++) {
        ast_script_stmt_visit(script, script->statements.data[i], global_scope);
    }

    script->vars_count += global_scope->var_offset;
}
