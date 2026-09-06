#include "ast/facts.h"

#define FACTS_INITIAL_CAPACITY 64

void facts_init(Facts *facts, Arena *arena) {
    Allocator allocator = arena_allocator(arena);

    expr_fact_init_alloc(&facts->types, allocator, FACTS_INITIAL_CAPACITY);
    expr_bind_init_alloc(&facts->uses, allocator, FACTS_INITIAL_CAPACITY);
    expr_callee_init_alloc(&facts->callees, allocator, FACTS_INITIAL_CAPACITY);
    expr_move_init_alloc(&facts->moves, allocator, FACTS_INITIAL_CAPACITY);
    expr_adjust_init_alloc(&facts->adjustments, allocator, FACTS_INITIAL_CAPACITY);
    expr_call_init_alloc(&facts->calls, allocator, FACTS_INITIAL_CAPACITY);
    stmt_fact_init_alloc(&facts->returns, allocator, FACTS_INITIAL_CAPACITY);
}

#define FACT_SETTER(fn, alias, map, KeyType, ValueType)                                                      \
    void fn(Facts *facts, KeyType key, ValueType value) {                                                    \
        ValueType *slot = alias##_lookup(&facts->map, key);                                                  \
                                                                                                             \
        if (slot) {                                                                                          \
            *slot = value;                                                                                   \
            return;                                                                                          \
        }                                                                                                    \
                                                                                                             \
        alias##_insert(&facts->map, key, value);                                                             \
    }

FACT_SETTER(fact_set_type, expr_fact, types, const ASTExpr *, const Type *)
FACT_SETTER(fact_set_use, expr_bind, uses, const ASTExpr *, Binding *)
FACT_SETTER(fact_set_callee, expr_callee, callees, const ASTExpr *, Function *)
FACT_SETTER(fact_set_moves, expr_move, moves, const ASTExpr *, bool)
FACT_SETTER(fact_set_adjustment, expr_adjust, adjustments, const ASTExpr *, Adjustment)
FACT_SETTER(fact_set_call_kind, expr_call, calls, const ASTExpr *, CallKind)
FACT_SETTER(fact_set_return_type, stmt_fact, returns, const ASTStmt *, const Type *)

const Type *fact_type_of(const Facts *facts, const ASTExpr *expr) {
    const Type **type = expr_fact_lookup((ExprTypeMap *)&facts->types, expr);

    return type ? *type : NULL;
}

Binding *fact_use_of(const Facts *facts, const ASTExpr *expr) {
    Binding **binding = expr_bind_lookup((ExprBindMap *)&facts->uses, expr);

    return binding ? *binding : NULL;
}

Function *fact_callee_of(const Facts *facts, const ASTExpr *expr) {
    Function **callee = expr_callee_lookup((ExprCalleeMap *)&facts->callees, expr);

    return callee ? *callee : NULL;
}

bool fact_moves(const Facts *facts, const ASTExpr *expr) {
    bool *moves = expr_move_lookup((ExprMoveMap *)&facts->moves, expr);

    return moves ? *moves : false;
}

CallKind fact_call_kind(const Facts *facts, const ASTExpr *expr) {
    CallKind *kind = expr_call_lookup((ExprCallMap *)&facts->calls, expr);

    return kind ? *kind : CALL_FUNCTION;
}

Adjustment fact_adjustment(const Facts *facts, const ASTExpr *expr) {
    Adjustment *adjustment = expr_adjust_lookup((ExprAdjustMap *)&facts->adjustments, expr);

    return adjustment ? *adjustment : (Adjustment){.kind = ADJUST_NONE};
}

const Type *fact_adjusted_type_of(const Facts *facts, const ASTExpr *expr) {
    Adjustment *adjustment = expr_adjust_lookup((ExprAdjustMap *)&facts->adjustments, expr);

    return adjustment && adjustment->to ? adjustment->to : fact_type_of(facts, expr);
}

const Type *fact_return_type_of(const Facts *facts, const ASTStmt *stmt) {
    const Type **type = stmt_fact_lookup((StmtTypeMap *)&facts->returns, stmt);

    return type ? *type : NULL;
}

Binding *fact_root_local(const Facts *facts, const ASTExpr *expr) {
    while (expr) {
        switch (expr->kind) {
        case EXPR_VARIABLE:
            return fact_use_of(facts, expr);
        case EXPR_FIELD:
            expr = expr->field.target;
            break;
        case EXPR_INDEX:
            expr = expr->index.target;
            break;
        case EXPR_DEREF:
        case EXPR_ADDR_OF:
            expr = expr->unary.target;
            break;
        default:
            return NULL;
        }
    }

    return NULL;
}
