#include "ast/check.h"

#include "type/type.h"

/* Whether one type may stand where another is wanted, and what the value must do to get there:
 * dereferenced to reach it, borrowed into a reference, or widened from an array to a slice. */

bool is_addressable(ResolverState *state, const ASTExpr *expr) {
    switch (expr->kind) {
    case EXPR_NAME:
        return fact_use_of(state->facts, expr) && fact_use_of(state->facts, expr)->kind == SYMBOL_VAR;
    case EXPR_FIELD:
        return is_addressable(state, expr->field.target);
    case EXPR_INDEX:

        return is_addressable(state, expr->index.target);
    case EXPR_DEREF:
        return true;
    default:
        return false;
    }
}

const Type *receiver_base_type(const Type *type) {
    while (type_is_indirect(type)) {
        type = type_pointee(type);
    }

    return type;
}

bool reads_as_a_view(TypeRegistry *registry, const Type *to, const Type *from) {
    const Type *view = type_registry_deref_of(registry, from);

    return view && type_kind(to) == TYPE_REF && view == type_pointee(to);
}

const Type *derefs_to(TypeRegistry *registry, const Type *type) {
    return type_registry_deref_of(registry, type);
}

bool lends_by_pointer(const Type *to, const Type *from) {
    return type_kind(to) == TYPE_REF && type_is_indirect(from) && type_pointee(to) == type_pointee(from);
}

bool accepts_by_borrowing(const Type *to, const Type *from) {
    return to != from && type_kind(to) == TYPE_REF && type_pointee(to) == from;
}

/* An array reaches a '&slice<T>' by handing over where it starts and how many it holds. */
bool unsizes_to_a_slice(const Type *to, const Type *from) {
    if (type_kind(to) != TYPE_REF || type_kind(type_pointee(to)) != TYPE_SLICE) {
        return false;
    }

    while (type_is_indirect(from)) {
        from = type_pointee(from);
    }

    return type_kind(from) == TYPE_ARRAY && type_array_element(from) == type_slice_element(type_pointee(to));
}

bool type_accepts(TypeRegistry *registry, const Type *to, const Type *from) {
    if (to == from) {
        return true;
    }

    if (unsizes_to_a_slice(to, from)) {
        return true;
    }

    if (accepts_by_borrowing(to, from)) {
        return true;
    }

    for (const Type *at = from;; at = type_pointee(at)) {
        if (reads_as_a_view(registry, to, at) || lends_by_pointer(to, at)) {
            return true;
        }

        if (!type_is_indirect(at)) {
            return false;
        }
    }
}

/* Records the dereferences a coercion applies, naming the type each one reaches. */
void adjust_derefs(ResolverState *state, Adjustment *adjustment, const Type *from, unsigned int count) {
    adjustment->derefs = count;
    adjustment->deref_types = count ? arena_alloc(state->global->arena, count * sizeof(const Type *)) : NULL;

    for (unsigned int i = 0; i < count; i++) {
        from = type_pointee(from);
        adjustment->deref_types[i] = from;
    }
}

static bool unsize_into(ResolverState *state, ASTExpr *expr, const Type *destination, Span span) {
    const Type *array = fact_type_of(state->facts, expr);
    unsigned int derefs = 0;

    while (type_is_indirect(array)) {
        array = type_pointee(array);
        derefs++;
    }

    if (!is_addressable(state, expr)) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "cannot borrow a temporary; bind it to a variable first");
        return false;
    }

    Symbol *addressed = fact_root_local(state->facts, expr);

    if (addressed) {
        addressed->pinned = true;
    }

    Adjustment adjustment = {.kind = ADJUST_UNSIZE,
                             .to = destination,
                             .length = type_array_length_is_known(array) ? type_array_length(array) : 1};

    adjust_derefs(state, &adjustment, fact_type_of(state->facts, expr), derefs);
    fact_set_adjustment(state->facts, expr, adjustment);

    return true;
}

bool borrow_into(ResolverState *state, ASTExpr *expr, const Type *destination, Span span) {
    const Type *from = fact_type_of(state->facts, expr);

    if (unsizes_to_a_slice(destination, from)) {
        return unsize_into(state, expr, destination, span);
    }

    const Type *at = from;
    unsigned int derefs = 0;

    while (type_is_indirect(at) && type_pointee(destination) != type_pointee(at) &&
           type_pointee(destination) != at) {
        at = type_pointee(at);
        derefs++;
    }

    if (!accepts_by_borrowing(destination, at)) {
        /* The dereferences alone reach what the destination takes, so they stand as the coercion. */
        if (derefs > 0) {
            Adjustment adjustment = {.kind = ADJUST_NONE, .to = at};

            adjust_derefs(state, &adjustment, from, derefs);
            fact_set_adjustment(state->facts, expr, adjustment);
        }

        return true;
    }

    if (!is_addressable(state, expr)) {
        diag_error(state->global->diagnostics, GAB_ERR_TYPE, span,
                   "cannot borrow a temporary; bind it to a variable first");
        return false;
    }

    Symbol *addressed = fact_root_local(state->facts, expr);

    if (addressed) {
        addressed->pinned = true;
    }

    Adjustment adjustment = {.kind = ADJUST_BORROW, .to = destination};

    adjust_derefs(state, &adjustment, from, derefs);
    fact_set_adjustment(state->facts, expr, adjustment);

    return true;
}

void mark_implicit_move(ResolverState *state, ASTExpr *value, const Type *destination, Span span) {
    if (!value || is_error_type(fact_type_of(state->facts, value))) {
        return;
    }

    if (type_registry_copies(state->global->types, fact_type_of(state->facts, value))) {
        return;
    }

    if (destination && !type_registry_owns(state->global->types, destination)) {
        return;
    }

    if (value->kind == EXPR_FIELD) {
        diag_error(state->global->diagnostics, GAB_ERR_LIFETIME, span,
                   "a field cannot be given up on its own; bind the whole struct instead");
        return;
    }

    if (value->kind == EXPR_INDEX) {
        diag_error(state->global->diagnostics, GAB_ERR_LIFETIME, span,
                   "an element cannot be given up on its own; bind the whole array instead");
        return;
    }

    Symbol *named = fact_use_of(state->facts, value);

    if (!named || named->kind != SYMBOL_VAR) {
        return;
    }

    fact_set_moves(state->facts, value, true);
}
