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

    Diagnostics *diagnostics;
} ResolverState;

static Type *resolver_error_type(ResolverState *state) {
    return type_registry_error_type(state->current_scope->type_registry);
}

static String *resolver_intern(ResolverState *state, StringRef ref) {
    return string_from_ref(state->current_scope->strings, ref);
}

// A type that is already poisoned had its error reported at the origin, so any
// further check involving it silently succeeds rather than cascading.
static bool is_error_type(Type *type) { return !type || type->kind == TYPE_ERROR; }

static const char *type_name(Type *type) {
    if (!type) {
        return "none";
    }

    return type->name->data;
}

void resolver_enter_scope(ResolverState *state) {
    state->current_scope = scope_create(state->arena, state->current_scope->strings, state->current_scope);
}

void resolver_exit_scope(ResolverState *state) { state->current_scope = state->current_scope->parent; }

static const char *bin_op_name(BinOp op) {
    switch (op) {
    case BIN_OP_ADD:
        return "+";
    case BIN_OP_SUB:
        return "-";
    case BIN_OP_MUL:
        return "*";
    case BIN_OP_DIV:
        return "/";
    case BIN_OP_LESS:
        return "<";
    case BIN_OP_GREATER:
        return ">";
    case BIN_OP_EQUAL:
        return "==";
    case BIN_OP_NEQUAL:
        return "!=";
    case BIN_OP_LEQUAL:
        return "<=";
    case BIN_OP_GEQUAL:
        return ">=";
    case BIN_OP_AND:
        return "&&";
    case BIN_OP_OR:
        return "||";
    }

    return "?";
}

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

        if (is_error_type(left_type) || is_error_type(right_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        const char *op_name = bin_op_name(expr->bin_op.op);

        if (left_type != right_type) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "cannot apply '%s' to %s and %s",
                       op_name, type_name(left_type), type_name(right_type));
            expr->type = resolver_error_type(state);
            break;
        }

        switch (expr->bin_op.op) {
        case BIN_OP_ADD:
        case BIN_OP_SUB:
        case BIN_OP_MUL:
        case BIN_OP_DIV:
            if (!is_numeric_type(left_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'%s' requires a numeric type, found %s", op_name, type_name(left_type));
                expr->type = resolver_error_type(state);
                return;
            }

            expr->type = left_type;
            return;
        case BIN_OP_EQUAL:
        case BIN_OP_NEQUAL:
            if (!is_comparable_type(left_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "'%s' is not supported for %s",
                           op_name, type_name(left_type));
                expr->type = resolver_error_type(state);
                return;
            }
            break;
        case BIN_OP_LESS:
        case BIN_OP_GREATER:
        case BIN_OP_LEQUAL:
        case BIN_OP_GEQUAL:
            if (!is_ordered_type(left_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'%s' requires an ordered type, found %s", op_name, type_name(left_type));
                expr->type = resolver_error_type(state);
                return;
            }
            break;
        case BIN_OP_AND:
        case BIN_OP_OR:
            if (!is_boolean_type(left_type)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                           "'%s' requires a boolean type, found %s", op_name, type_name(left_type));
                expr->type = resolver_error_type(state);
                return;
            }
            break;
        }

        expr->type = type_registry_get_builtin(state->current_scope->type_registry, TYPE_BOOL);
        break;
    }
    case EXPR_VARIABLE: {
        Symbol *entry = scope_symbol_lookup(state->current_scope, resolver_intern(state, expr->var.name));

        if (!entry) {
            char *name = string_ref_to_cstr(expr->var.name);
            diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "undeclared variable '%s'", name);
            free(name);

            expr->type = resolver_error_type(state);
            break;
        }

        expr->symbol = entry;
        expr->type = entry->var.type;
        break;
    }
    case EXPR_CALL: {
        ast_script_expr_visit(state, expr->call.target);

        for (size_t i = 0; i < expr->call.args.size; i++) {
            ast_script_expr_visit(state, expr->call.args.data[i]);
        }

        Symbol *callee = expr->call.target->symbol;

        if (!callee) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (callee->kind != SYMBOL_FUNC) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "this expression is not callable");
            expr->type = resolver_error_type(state);
            break;
        }

        if (expr->call.args.size != callee->func.param_count) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span, "expected %zu argument(s), found %zu",
                       callee->func.param_count, expr->call.args.size);
            expr->type = resolver_error_type(state);
            break;
        }

        for (size_t i = 0; i < expr->call.args.size; i++) {
            ASTExpr *arg = expr->call.args.data[i];
            Type *param_type = callee->func.params[i];

            if (is_error_type(arg->type) || is_error_type(param_type)) {
                continue;
            }

            if (arg->type != param_type) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, arg->span,
                           "argument %zu is %s, but %s was declared", i + 1, type_name(arg->type),
                           type_name(param_type));
            }
        }

        expr->symbol = callee;
        expr->type = callee->func.return_type;
        break;
    }
    case EXPR_FIELD: {
        ast_script_expr_visit(state, expr->field.target);

        Type *target_type = expr->field.target->type;

        if (is_error_type(target_type)) {
            expr->type = resolver_error_type(state);
            break;
        }

        if (target_type->kind != TYPE_STRUCT) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, expr->span,
                       "%s is not a struct, so it has no fields", type_name(target_type));
            expr->type = resolver_error_type(state);
            break;
        }

        String *field_name = resolver_intern(state, expr->field.name);
        const TypeField *field = type_find_field(target_type, field_name);

        if (!field) {
            diag_error(state->diagnostics, GAB_ERR_NAME, expr->span, "'%s' has no field '%s'",
                       type_name(target_type), field_name->data);
            expr->type = resolver_error_type(state);
            break;
        }

        expr->field.field = field;
        expr->type = field->type;

        // Field access addresses the target's slots, so it inherits the
        // target's symbol and stays assignable through the chain.
        expr->symbol = expr->field.target->symbol;
        break;
    }
    case EXPR_LITERAL: {
        expr->type = type_registry_get_builtin(state->current_scope->type_registry, expr->lit.kind);
        break;
    }
    default:
        break;
    }
}

// Returns NULL when there is no spec to resolve (an omitted type), and the
// poison type when the spec names something that does not exist.
Type *ast_script_resolve_type(ResolverState *state, TypeSpec *spec, Span span) {
    if (!spec) {
        return NULL;
    }

    TypeRegistry *registry = state->current_scope->type_registry;
    Type *type = type_registry_get(registry, resolver_intern(state, spec->name));

    if (!type) {
        char *name = string_ref_to_cstr(spec->name);
        diag_error(state->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name);
        free(name);

        return resolver_error_type(state);
    }

    for (unsigned int i = 0; i < spec->pointer_depth; i++) {
        type = type_registry_pointer_to(registry, type);
    }

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
            Type *decl_type = ast_script_resolve_type(state, stmt->var_decl.type_spec, stmt->span);

            if (stmt->var_decl.initializer) {
                Type *init_type = stmt->var_decl.initializer->type;

                if (!is_error_type(decl_type) && !is_error_type(init_type) && decl_type != init_type) {
                    diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->var_decl.initializer->span,
                               "cannot initialize a variable of type %s with a value of type %s",
                               type_name(decl_type), type_name(init_type));
                    decl_type = resolver_error_type(state);
                }
            }

            type = decl_type;
        } else if (stmt->var_decl.initializer) {
            type = stmt->var_decl.initializer->type;
        } else {
            type = resolver_error_type(state);
        }

        Symbol *var = scope_decl_var(state->current_scope, resolver_intern(state, stmt->var_decl.name), type);

        if (!var) {
            char *name = string_ref_to_cstr(stmt->var_decl.name);
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared in this scope",
                       name);
            free(name);
            break;
        }

        stmt->var_decl.symbol = var;
        break;
    }
    case STMT_FUNC_DECL: {
        StringRef func_name = stmt->func_decl.name;
        Type *func_return_type = ast_script_resolve_type(state, stmt->func_decl.return_type, stmt->span);

        // Declared in the enclosing scope, before the body is visited, so that
        // callers and the function itself can both see it.
        Symbol *func =
            scope_decl_func(state->current_scope, resolver_intern(state, func_name), func_return_type);

        if (!func) {
            char *name = string_ref_to_cstr(func_name);
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "'%s' is already declared in this scope",
                       name);
            free(name);
        }

        stmt->func_decl.symbol = func;

        size_t param_count = stmt->func_decl.params.size;

        if (func && param_count > 0) {
            func->func.params = arena_alloc(state->arena, param_count * sizeof(Type *));
            func->func.param_count = param_count;
        }

        resolver_enter_scope(state);

        for (size_t i = 0; i < param_count; i++) {
            ASTField *param = stmt->func_decl.params.data[i];

            String *param_name = resolver_intern(state, param->name);
            Type *param_type = ast_script_resolve_type(state, param->type_spec, param->span);

            if (func) {
                func->func.params[i] = param_type;
            }

            Symbol *symbol = scope_decl_var(state->current_scope, param_name, param_type);

            if (!symbol) {
                char *name = string_ref_to_cstr(param->name);
                diag_error(state->diagnostics, GAB_ERR_NAME, param->span, "duplicate parameter '%s'", name);
                free(name);
                continue;
            }

            param->symbol = symbol;
        }

        FuncContext previous_context = state->func_context;

        state->func_context.return_type = func_return_type;

        ast_script_stmt_visit(state, stmt->func_decl.body);

        state->func_context = previous_context;

        resolver_exit_scope(state);
        break;
    }
    case STMT_STRUCT_DECL: {
        String *struct_name = resolver_intern(state, stmt->struct_decl.name);
        TypeRegistry *registry = state->current_scope->type_registry;

        if (type_registry_get(registry, struct_name)) {
            diag_error(state->diagnostics, GAB_ERR_NAME, stmt->span, "type '%s' is already declared",
                       struct_name->data);
            break;
        }

        Type *type = type_struct_create(state->arena, struct_name, stmt->struct_decl.fields.size);
        bool poisoned = false;

        for (size_t i = 0; i < stmt->struct_decl.fields.size; i++) {
            ASTField *field = stmt->struct_decl.fields.data[i];
            String *field_name = resolver_intern(state, field->name);

            if (type_find_field(type, field_name)) {
                diag_error(state->diagnostics, GAB_ERR_NAME, field->span,
                           "duplicate field '%s' in struct '%s'", field_name->data, struct_name->data);
                poisoned = true;
                continue;
            }

            // The struct is registered only after its fields resolve, so a
            // self-reference would otherwise surface as "unknown type". A
            // pointer to self is not containment, so only depth 0 is rejected.
            if (field->type_spec->pointer_depth == 0 &&
                string_ref_equals_cstr(field->type_spec->name, struct_name->data)) {
                diag_error(state->diagnostics, GAB_ERR_TYPE, field->span, "struct '%s' cannot contain itself",
                           struct_name->data);
                poisoned = true;
                continue;
            }

            Type *field_type = ast_script_resolve_type(state, field->type_spec, field->span);

            if (is_error_type(field_type)) {
                poisoned = true;
                continue;
            }

            type_add_field(type, field_name, field_type);
        }

        if (poisoned) {
            break;
        }

        type_layout_compute(type);
        type_registry_register(registry, type);

        stmt->struct_decl.type = type;
        break;
    }
    case STMT_ASSIGN: {
        ast_script_expr_visit(state, stmt->assign.target);
        ast_script_expr_visit(state, stmt->assign.value);

        Type *target_type = stmt->assign.target->type;
        Type *value_type = stmt->assign.value->type;

        if (!is_error_type(target_type) && !is_error_type(value_type) && target_type != value_type) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span,
                       "cannot assign a value of type %s to a target of type %s", type_name(value_type),
                       type_name(target_type));
        }
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

        // A NULL type here means "no value", which is a distinct case from a
        // poisoned one: it must still be checked against the declared type.
        bool poisoned = (expected && expected->kind == TYPE_ERROR) || (actual && actual->kind == TYPE_ERROR);

        if (!poisoned && actual != expected) {
            diag_error(state->diagnostics, GAB_ERR_TYPE, stmt->span, "returns %s, but %s was declared",
                       type_name(actual), type_name(expected));
        }
        break;
    }
    }
}

bool ast_script_resolve(Arena *arena, ASTScript *script, Scope *global_scope, Diagnostics *diagnostics) {
    ResolverState state = {
        .arena = arena,
        .global_scope = global_scope,
        .current_scope = global_scope,
        .func_context =
            {
                .return_type = NULL,
            },
        .diagnostics = diagnostics,
    };

    size_t errors_before = diagnostics_count(diagnostics);

    for (int i = 0; i < script->statements.size; i++) {
        ast_script_stmt_visit(&state, script->statements.data[i]);
    }

    return diagnostics_count(diagnostics) == errors_before;
}
