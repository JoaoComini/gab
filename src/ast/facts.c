#include "ast/facts.h"

#define FACTS_INITIAL_CAPACITY 64

void facts_init(Facts *facts, Arena *arena) {
    Allocator allocator = arena_allocator(arena);

    expr_fact_init_alloc(&facts->exprs, allocator, FACTS_INITIAL_CAPACITY);
    stmt_fact_init_alloc(&facts->stmts, allocator, FACTS_INITIAL_CAPACITY);
    def_fact_init_alloc(&facts->defs, allocator, FACTS_INITIAL_CAPACITY);
}

/* The record for a node, created empty where resolution has concluded nothing about it yet. */
static ExprFact *fact_mut(Facts *facts, const ASTExpr *expr) {
    ExprFact *fact = expr_fact_lookup(&facts->exprs, expr);

    return fact ? fact : expr_fact_insert(&facts->exprs, expr, (ExprFact){0});
}

const ExprFact *fact_of(const Facts *facts, const ASTExpr *expr) {
    return expr_fact_lookup((ExprFactMap *)&facts->exprs, expr);
}

void fact_set_type(Facts *facts, const ASTExpr *expr, const Type *type) {
    fact_mut(facts, expr)->type = type;
}

void fact_set_use(Facts *facts, const ASTExpr *expr, Symbol *binding) {
    fact_mut(facts, expr)->use = binding;
}

void fact_set_callee(Facts *facts, const ASTExpr *expr, Function *callee) {
    fact_mut(facts, expr)->callee = callee;
}

void fact_set_moves(Facts *facts, const ASTExpr *expr, bool moves) { fact_mut(facts, expr)->moves = moves; }

void fact_set_adjustment(Facts *facts, const ASTExpr *expr, Adjustment adjustment) {
    fact_mut(facts, expr)->adjustment = adjustment;
}

void fact_set_call_kind(Facts *facts, const ASTExpr *expr, CallKind kind) {
    fact_mut(facts, expr)->call = kind;
}

void fact_set_constant(Facts *facts, const ASTExpr *expr, Constant constant) {
    ExprFact *fact = fact_mut(facts, expr);

    fact->constant = constant;
    fact->has_constant = true;
}

void fact_set_field(Facts *facts, const ASTExpr *expr, size_t field) { fact_mut(facts, expr)->field = field; }

void fact_set_initialized_field(Facts *facts, const ASTExpr *value, size_t field) {
    fact_mut(facts, value)->initialized_field = field;
}

static StmtFact *stmt_fact_mut(Facts *facts, const ASTStmt *stmt) {
    StmtFact *fact = stmt_fact_lookup(&facts->stmts, stmt);

    return fact ? fact : stmt_fact_insert(&facts->stmts, stmt, (StmtFact){0});
}

void fact_set_return_type(Facts *facts, const ASTStmt *stmt, const Type *type) {
    stmt_fact_mut(facts, stmt)->return_type = type;
}

void fact_set_function(Facts *facts, const ASTStmt *stmt, Function *function) {
    stmt_fact_mut(facts, stmt)->function = function;
}

void fact_set_def(Facts *facts, const ASTIdent *name, Symbol *binding) {
    Symbol **slot = def_fact_lookup(&facts->defs, name);

    if (slot) {
        *slot = binding;
        return;
    }

    def_fact_insert(&facts->defs, name, binding);
}

Function *fact_function_of(const Facts *facts, const ASTStmt *stmt) {
    const StmtFact *fact = stmt_fact_lookup((StmtFactMap *)&facts->stmts, stmt);

    return fact ? fact->function : NULL;
}

Symbol *fact_def_of(const Facts *facts, const ASTIdent *name) {
    Symbol **slot = def_fact_lookup((DefMap *)&facts->defs, name);

    return slot ? *slot : NULL;
}

bool fact_constant_of(const Facts *facts, const ASTExpr *expr, Constant *out) {
    const ExprFact *fact = fact_of(facts, expr);

    if (fact && fact->has_constant && out) {
        *out = fact->constant;
    }

    return fact && fact->has_constant;
}

const Type *fact_type_of(const Facts *facts, const ASTExpr *expr) {
    const ExprFact *fact = fact_of(facts, expr);

    return fact ? fact->type : NULL;
}

Symbol *fact_use_of(const Facts *facts, const ASTExpr *expr) {
    const ExprFact *fact = fact_of(facts, expr);

    return fact ? fact->use : NULL;
}

Function *fact_callee_of(const Facts *facts, const ASTExpr *expr) {
    const ExprFact *fact = fact_of(facts, expr);

    return fact ? fact->callee : NULL;
}

bool fact_moves(const Facts *facts, const ASTExpr *expr) {
    const ExprFact *fact = fact_of(facts, expr);

    return fact ? fact->moves : false;
}

CallKind fact_call_kind(const Facts *facts, const ASTExpr *expr) {
    const ExprFact *fact = fact_of(facts, expr);

    return fact ? fact->call : CALL_FUNCTION;
}

size_t fact_field_of(const Facts *facts, const ASTExpr *expr) {
    const ExprFact *fact = fact_of(facts, expr);

    return fact ? fact->field : 0;
}

size_t fact_initialized_field_of(const Facts *facts, const ASTExpr *value) {
    const ExprFact *fact = fact_of(facts, value);

    return fact ? fact->initialized_field : 0;
}

Adjustment fact_adjustment(const Facts *facts, const ASTExpr *expr) {
    const ExprFact *fact = fact_of(facts, expr);

    return fact ? fact->adjustment : (Adjustment){.kind = ADJUST_NONE};
}

const Type *fact_adjusted_type_of(const Facts *facts, const ASTExpr *expr) {
    const ExprFact *fact = fact_of(facts, expr);

    if (!fact) {
        return NULL;
    }

    return fact->adjustment.to ? fact->adjustment.to : fact->type;
}

const Type *fact_return_type_of(const Facts *facts, const ASTStmt *stmt) {
    const StmtFact *fact = stmt_fact_lookup((StmtFactMap *)&facts->stmts, stmt);

    return fact ? fact->return_type : NULL;
}

Symbol *fact_root_local(const Facts *facts, const ASTExpr *expr) {
    while (expr) {
        switch (expr->kind) {
        case EXPR_NAME:
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
