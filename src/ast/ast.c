#include "ast.h"

#include "ast/expr.h"
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

    return script;
}

void ast_script_destroy(ASTScript *script) {
    ast_stmt_list_free(&script->statements);

    free(script);
}

void ast_script_add_statement(ASTScript *script, ASTStmt *stmt) {
    ast_stmt_list_add(&script->statements, stmt);
}

typedef struct {
    Type *return_type;
} FuncContext;

typedef struct {
    Arena *arena;

    Scope *global_scope;
    Scope *current_scope;

    FuncContext func_context;
} ResolverState;

void resolver_enter_scope(ResolverState *state) {
    state->current_scope = scope_create(state->arena, state->current_scope);
}

void resolver_exit_scope(ResolverState *state) { state->current_scope = state->current_scope->parent; }

bool is_numeric_type(Type *t) { return t->kind == TYPE_INT || t->kind == TYPE_FLOAT; }

bool is_boolean_type(Type *t) { return t->kind == TYPE_BOOL; }

bool is_ordered_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

bool is_comparable_type(Type *t) { return is_numeric_type(t) || is_boolean_type(t); }

void ast_script_expr_visit(ResolverState *state, ASTExpr *expr) {
    if (!expr) {
        return;
    }

    switch (expr->kind) {
    case EXPR_BIN_OP: {
        ast_script_expr_visit(state, expr->bin_op.left);
        ast_script_expr_visit(state, expr->bin_op.right);

        Type *left_type = expr->bin_op.left->type;
        Type *right_type = expr->bin_op.right->type;

        assert(left_type == right_type && "mismatched types in binary operation");

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

        expr->type = type_registry_get_builtin(state->current_scope->type_registry, TYPE_BOOL);
        break;
    }
    case EXPR_IDENTIFIER: {
        Symbol *entry = scope_symbol_lookup(state->current_scope, string_from_ref(expr->ident.name));
        assert(entry && "variable not found in this scope");

        expr->symbol = entry;
        expr->type = entry->var.type;
        break;
    }
    case EXPR_LITERAL: {
        expr->type = type_registry_get_builtin(state->current_scope->type_registry, expr->lit.kind);
        break;
    }
    case EXPR_CALL: {
        assert(expr->call.target->kind == EXPR_IDENTIFIER);

        Symbol *entry =
            scope_symbol_lookup(state->current_scope, string_from_ref(expr->call.target->ident.name));

        assert(entry && "function not found in this scope");

        assert(entry->kind == SYMBOL_FUNC && "expression is not callable");

        size_t arg_count = expr->call.params.size;
        size_t param_count = entry->func.signature->params.size;

        assert(arg_count == param_count && "unexpected number of arguments");

        for (size_t i = 0; i < arg_count; i++) {
            ASTExpr *arg = ast_expr_list_get(&expr->call.params, i);
            ast_script_expr_visit(state, arg);

            Type *type = type_list_get(&entry->func.signature->params, i);

            assert(type == arg->type && "mismatched types on function arguments");
        }

        expr->type = entry->func.signature->return_type;
        expr->symbol = entry;
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

void ast_script_stmt_visit(ResolverState *state, ASTStmt *stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
    case STMT_EXPR: {
        ast_script_expr_visit(state, stmt->expr.value);
        break;
    }
    case STMT_VAR_DECL: {
        ast_script_expr_visit(state, stmt->var_decl.initializer);

        Type *type;
        if (stmt->var_decl.type_spec) {
            Type *decl_type = ast_script_resolve_type(state->current_scope, stmt->var_decl.type_spec);

            if (stmt->var_decl.initializer) {
                Type *init_type = stmt->var_decl.initializer->type;
                assert(decl_type == init_type && "mismatched types in assignment");
            }

            type = decl_type;
        } else {
            type = stmt->var_decl.initializer->type;
        }

        Symbol *var = scope_decl_var(state->current_scope, string_from_ref(stmt->var_decl.name), type);
        assert(var && "variable already declared in this scope");

        stmt->var_decl.symbol = var;
        break;
    }
    case STMT_FUNC_DECL: {
        resolver_enter_scope(state);

        TypeList params = type_list_create();
        for (int i = 0; i < stmt->func_decl.params.size; i++) {
            ASTField *param = stmt->func_decl.params.data[i];

            String *param_name = string_from_ref(param->name);
            Type *param_type = ast_script_resolve_type(state->current_scope, param->type_spec);

            Symbol *symbol = scope_decl_var(state->current_scope, param_name, param_type);
            assert(symbol && "variable already declared in this scope");

            type_list_add(&params, param_type);

            param->symbol = symbol;
        }

        StringRef func_name = stmt->func_decl.name;
        Type *func_return_type = ast_script_resolve_type(state->current_scope, stmt->func_decl.return_type);
        FuncContext previous_context = state->func_context;

        state->func_context.return_type = func_return_type;

        ast_script_stmt_visit(state, stmt->func_decl.body);

        state->func_context = previous_context;

        resolver_exit_scope(state);

        Symbol *func =
            scope_decl_func(state->current_scope, string_from_ref(func_name), params, func_return_type);
        assert(func && "function already declared in this scope");

        stmt->func_decl.symbol = func;

        break;
    }
    case STMT_ASSIGN: {
        ast_script_expr_visit(state, stmt->assign.target);
        ast_script_expr_visit(state, stmt->assign.value);

        Type *target_type = stmt->assign.target->type;
        Type *value_type = stmt->assign.value->type;

        assert(target_type == value_type && "mismatched types in assignment");
        break;
    }
    case STMT_IF: {
        ast_script_expr_visit(state, stmt->ifstmt.condition);
        ast_script_stmt_visit(state, stmt->ifstmt.then_block);
        ast_script_stmt_visit(state, stmt->ifstmt.else_block);
        break;
    }
    case STMT_BLOCK: {
        resolver_enter_scope(state);

        for (int i = 0; i < stmt->block.list.size; i++) {
            ast_script_stmt_visit(state, stmt->block.list.data[i]);
        }

        resolver_exit_scope(state);
        break;
    }
    case STMT_RETURN: {
        ast_script_expr_visit(state, stmt->ret.result);

        Type *expected = state->func_context.return_type;
        Type *actual = stmt->ret.result ? stmt->ret.result->type : NULL;

        assert(actual == expected && "return type does not match function return type");
        break;
    }
    }
}

void ast_script_resolve(Arena *arena, ASTScript *script, Scope *global_scope) {
    ResolverState state = {
        .arena = arena,
        .global_scope = global_scope,
        .current_scope = global_scope,
        .func_context =
            {
                .return_type = NULL,
            },
    };

    for (int i = 0; i < script->statements.size; i++) {
        ast_script_stmt_visit(&state, script->statements.data[i]);
    }
}
