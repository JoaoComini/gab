#include "ast/check.h"

#include "string/string_ref.h"
#include "type/type.h"

#include <stdlib.h>

/* What a written type denotes: the primitives and the types a scope names, the runs and slices the
 * language supplies, and the arguments a generic is applied to. */

bool reject_unsized(ResolverState *state, const Type *type, Span span, const char *held_as) {
    if (!type || type_is_sized(type)) {
        return false;
    }

    diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
               "nothing holds a '%s', so it cannot be %s; write '&%s'", type_name(state, type), held_as,
               type_name(state, type));
    return true;
}

/* An element must be sized and non-recursive wherever a run of it is laid out. */
const Type *resolve_element_type(ResolverState *state, TypeExpr *expr, Span span, const char *held_as) {
    const Type *element = resolve_type_expr(state, expr, span);

    if (is_error_type(element)) {
        return NULL;
    }

    if (type_has_param(element)) {
        return element;
    }

    if (reject_unsized(state, element, span, held_as)) {
        return NULL;
    }

    StructDecl *cycle = element_completes_a_cycle(state, element);

    if (cycle) {
        report_containment_cycle(state, cycle, span);
        return NULL;
    }

    if (type_registry_size_of(state->global->types, element) == 0) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span, "%s must have a size", held_as);
        return NULL;
    }

    return element;
}

/* Which kind of parameter a bound declares, which its syntax alone says: nothing here is resolved, so
 * this answers before the parameters are in scope and their bounds can be. 'N: i32' declares a value
 * parameter, as Rust spells 'const N: usize'; any other bound names an interface. */
BoundKind bound_kind_of(const TypeRegistry *registry, StringPool *strings, const TypeExpr *bound) {
    if (!bound) {
        return BOUND_NONE;
    }

    return bound->kind == TYPE_EXPR_NAME &&
                   string_from_ref(strings, bound->name) == type_registry_names(registry)->i32
               ? BOUND_VALUE
               : BOUND_INTERFACE;
}

bool bind_type_param(TypeRegistry *registry, Scope *params, String *name, size_t index, BoundKind kind) {
    if (kind == BOUND_VALUE) {
        return scope_bind_const(params, name, (TypeConst){.kind = CONST_PARAM, .param = index});
    }

    return scope_bind_type_param(params, name, type_registry_param(registry, index));
}

/* A length is written as a literal, or named as the value parameter a generic declaration takes. */
static bool resolve_array_length(ResolverState *state, TypeExpr *expr, Span span, TypeArg *out) {
    if (expr->kind == TYPE_EXPR_CONST) {
        Constant length = constant_int(i32_type(state), expr->constant);

        *out = (TypeArg){.kind = TYPE_ARG_CONST, .constant = {.kind = CONST_VALUE, .value = length}};
        return true;
    }

    if (expr->kind == TYPE_EXPR_NAME) {
        Scope *scope = resolver_expr_scope(state, expr->name);
        Symbol *symbol = resolver_resolve_name(state, scope, resolver_expr_member(state, expr->name));

        if (symbol && symbol->kind == SYMBOL_CONST) {
            *out = (TypeArg){.kind = TYPE_ARG_CONST, .constant = symbol->constant};
            return true;
        }
    }

    diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
               "an array's length is a literal or a value parameter, as 'array<i32, 3>'");
    return false;
}

static const Type *resolve_array_type(ResolverState *state, TypeExpr *expr, Span span) {
    TypeRegistry *registry = state->global->types;

    if (expr->apply.args.size != 2) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "'array' takes an element and a length, as 'array<i32, 3>'");
        return resolver_error_type(state);
    }

    const Type *element = resolve_element_type(state, expr->apply.args.data[0], span, "an array's element");

    TypeArg length;

    if (!element || !resolve_array_length(state, expr->apply.args.data[1], span, &length)) {
        return resolver_error_type(state);
    }

    if (length.constant.kind == CONST_PARAM) {
        return type_registry_array_with(registry, element, length);
    }

    if (length.constant.value.as_int <= 0) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "an array's length must be positive, not %d", length.constant.value.as_int);
        return resolver_error_type(state);
    }

    return type_registry_array_with(registry, element, length);
}

static const Type *resolve_slice_type(ResolverState *state, TypeExpr *expr, Span span) {
    if (expr->apply.args.size != 1 || expr->apply.args.data[0]->kind == TYPE_EXPR_CONST) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "'slice' takes one element type, as 'slice<int>'");
        return resolver_error_type(state);
    }

    const Type *element = resolve_element_type(state, expr->apply.args.data[0], span, "a slice's element");

    if (!element) {
        return resolver_error_type(state);
    }

    return type_registry_slice_of(state->global->types, element);
}

/* A raw run names where elements start and nothing more: no length, and nothing it owns. */
static const Type *resolve_raw_type(ResolverState *state, TypeExpr *expr, Span span) {
    if (expr->apply.args.size != 1 || expr->apply.args.data[0]->kind == TYPE_EXPR_CONST) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "'raw' takes one element type, as 'raw<i32>'");
        return resolver_error_type(state);
    }

    const Type *element = resolve_element_type(state, expr->apply.args.data[0], span, "a raw run's element");

    if (!element) {
        return resolver_error_type(state);
    }

    return type_registry_raw_of(state->global->types, element);
}

const Type *resolve_type_expr(ResolverState *state, TypeExpr *expr, Span span) {
    if (!expr) {
        return NULL;
    }

    TypeRegistry *registry = state->global->types;

    switch (expr->kind) {
    case TYPE_EXPR_BOX:
    case TYPE_EXPR_REF: {
        const Type *inner = resolve_type_expr(state, expr->indirect.inner, span);

        if (is_error_type(inner)) {
            return resolver_error_type(state);
        }

        return expr->kind == TYPE_EXPR_REF ? type_registry_ref_to(registry, inner)
                                           : type_registry_box_to(registry, inner);
    }

    case TYPE_EXPR_APPLY: {
        if (names_the_same(state, expr->apply.base->name, resolver_names(state)->array)) {
            return resolve_array_type(state, expr, span);
        }

        if (names_the_same(state, expr->apply.base->name, resolver_names(state)->slice)) {
            return resolve_slice_type(state, expr, span);
        }

        if (names_the_same(state, expr->apply.base->name, resolver_names(state)->raw)) {
            return resolve_raw_type(state, expr, span);
        }

        Scope *base_scope = resolver_expr_scope(state, expr->apply.base->name);

        String *base_name = base_scope ? resolver_expr_member(state, expr->apply.base->name) : NULL;

        Symbol *base_symbol = base_name ? resolver_resolve_name(state, base_scope, base_name) : NULL;

        const TypeDecl *base_decl =
            base_symbol && base_symbol->kind == SYMBOL_TYPE_DECL ? base_symbol->type_decl : NULL;
        const Type *base = symbol_type(registry, base_symbol);

        if (!base_symbol) {
            char *name = string_ref_to_cstr(expr->apply.base->name);
            diag_error(state->global->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name);
            free(name);

            return resolver_error_type(state);
        }

        if (base_decl) {
            if (expr->apply.args.size != base_decl->param_count) {
                diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                           "'%s' takes %zu type argument(s), not %zu", base_decl->id.name->data,
                           base_decl->param_count, expr->apply.args.size);
                return resolver_error_type(state);
            }

            const Type *args[GAB_MAX_TYPE_PARAMS];

            for (size_t i = 0; i < expr->apply.args.size; i++) {
                args[i] = resolve_type_expr(state, expr->apply.args.data[i], span);

                if (is_error_type(args[i])) {
                    return resolver_error_type(state);
                }

                if (type_has_param(args[i])) {
                    continue;
                }

                if (reject_unsized(state, args[i], span, "a type argument")) {
                    return resolver_error_type(state);
                }

                StructDecl *cycle = element_completes_a_cycle(state, args[i]);

                if (cycle) {
                    report_containment_cycle(state, cycle, span);
                    return resolver_error_type(state);
                }
            }

            return type_registry_apply(registry, base_decl, args, expr->apply.args.size);
        }

        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span, "%s does not take a type argument",
                   type_name(state, base));

        return resolver_error_type(state);
    }

    case TYPE_EXPR_CONST:
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "a length is an argument to 'array', not a type");
        return resolver_error_type(state);

    case TYPE_EXPR_NAME:
        break;
    }

    Scope *scope = resolver_expr_scope(state, expr->name);

    Symbol *symbol = resolver_resolve_name(state, scope, resolver_expr_member(state, expr->name));

    const Type *type = symbol_type(registry, symbol);

    if (type) {
        return type;
    }

    if (symbol && symbol->kind == SYMBOL_TYPE_DECL) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span, "'%s' takes %zu type argument(s), not 0",
                   symbol->type_decl->id.name->data, symbol->type_decl->param_count);

        return resolver_error_type(state);
    }

    if (resolver_intern(state, expr->name) == resolver_names(state)->self) {
        diag_error(state->global->diagnostics, GAB_ERR_NAME, span,
                   "'Self' names the type an 'impl' block is for, and there is none here");

        return resolver_error_type(state);
    }

    char *name_text = string_ref_to_cstr(expr->name);
    diag_error(state->global->diagnostics, GAB_ERR_NAME, span, "unknown type '%s'", name_text);
    free(name_text);

    return resolver_error_type(state);
}
