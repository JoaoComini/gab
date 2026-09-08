#ifndef GAB_AST_FACTS_H
#define GAB_AST_FACTS_H

#include "ast/expr.h"
#include "ast/stmt.h"
#include "binding.h"
#include "constant.h"
#include "memory/arena.h"
#include "type/type.h"
#include "util/hash_map.h"

/* A node is one object for the whole of a unit, so its address names it. */
#define expr_fact_hash(key) ((size_t)(key) >> 4)
#define expr_fact_key_equals(key, other) ((key) == (other))

#define expr_bind_hash(key) expr_fact_hash(key)
#define expr_bind_key_equals(key, other) ((key) == (other))

#define expr_callee_hash(key) expr_fact_hash(key)
#define expr_callee_key_equals(key, other) ((key) == (other))

#define expr_move_hash(key) expr_fact_hash(key)
#define expr_move_key_equals(key, other) ((key) == (other))

#define expr_call_hash(key) expr_fact_hash(key)
#define expr_call_key_equals(key, other) ((key) == (other))

#define expr_const_hash(key) expr_fact_hash(key)
#define expr_const_key_equals(key, other) ((key) == (other))

#define expr_adjust_hash(key) expr_fact_hash(key)
#define expr_adjust_key_equals(key, other) ((key) == (other))

#define stmt_fact_hash(key) expr_fact_hash(key)
#define stmt_fact_key_equals(key, other) ((key) == (other))

GAB_HASH_MAP(ExprTypeMap, expr_fact, const ASTExpr *, const Type *)
GAB_HASH_MAP(ExprBindMap, expr_bind, const ASTExpr *, Binding *)
GAB_HASH_MAP(ExprCalleeMap, expr_callee, const ASTExpr *, Function *)
GAB_HASH_MAP(ExprMoveMap, expr_move, const ASTExpr *, bool)
GAB_HASH_MAP(ExprConstMap, expr_const, const ASTExpr *, Constant)
GAB_HASH_MAP(StmtTypeMap, stmt_fact, const ASTStmt *, const Type *)

/* What a call names, which its target's resolution decides: a function to call, or a type to convert to. */
typedef enum {
    CALL_FUNCTION,
    CALL_CONVERSION,
} CallKind;

/* How a value reaches the type its destination takes: dereferenced some number of times, then
 * borrowed or widened to a slice. */
typedef enum {
    ADJUST_NONE,
    ADJUST_BORROW,
    ADJUST_UNSIZE,
} AdjustKind;

typedef struct {
    AdjustKind kind;

    /* Dereferences applied before the adjustment, innermost last. */
    unsigned int derefs;

    /* The type each dereference reaches, so lowering names them without recomputing the walk. */
    const Type **deref_types;

    const Type *to;

    union {
        /* The element count the slice is given. */
        int32_t length;
    };
} Adjustment;

GAB_HASH_MAP(ExprAdjustMap, expr_adjust, const ASTExpr *, Adjustment)
GAB_HASH_MAP(ExprCallMap, expr_call, const ASTExpr *, CallKind)

/* What resolution concluded about each node, which only what it hands on can read. */
typedef struct Facts {
    ExprTypeMap types;
    ExprBindMap uses;
    ExprCalleeMap callees;
    ExprMoveMap moves;
    ExprAdjustMap adjustments;
    ExprCallMap calls;

    /* What an expression was found to be worth, where resolution could answer it outright. */
    ExprConstMap constants;

    StmtTypeMap returns;
} Facts;

void facts_init(Facts *facts, Arena *arena);

void fact_set_type(Facts *facts, const ASTExpr *expr, const Type *type);
void fact_set_use(Facts *facts, const ASTExpr *expr, Binding *binding);
void fact_set_callee(Facts *facts, const ASTExpr *expr, Function *callee);
void fact_set_moves(Facts *facts, const ASTExpr *expr, bool moves);
void fact_set_adjustment(Facts *facts, const ASTExpr *expr, Adjustment adjustment);
void fact_set_call_kind(Facts *facts, const ASTExpr *expr, CallKind kind);
void fact_set_constant(Facts *facts, const ASTExpr *expr, Constant constant);
void fact_set_return_type(Facts *facts, const ASTStmt *stmt, const Type *type);

const Type *fact_type_of(const Facts *facts, const ASTExpr *expr);
Binding *fact_use_of(const Facts *facts, const ASTExpr *expr);
Function *fact_callee_of(const Facts *facts, const ASTExpr *expr);
bool fact_moves(const Facts *facts, const ASTExpr *expr);

/* Whether resolution answered this expression with a constant, which lowering emits rather than the node. */
bool fact_constant_of(const Facts *facts, const ASTExpr *expr, Constant *out);

/* The coercion a value needs where it sits; its kind is ADJUST_NONE where it needs none. */
Adjustment fact_adjustment(const Facts *facts, const ASTExpr *expr);

/* What a call names; a call resolution rejected never reaches a stage that asks. */
CallKind fact_call_kind(const Facts *facts, const ASTExpr *expr);

/* The type a value has once its coercion is applied, which is its own where it has none. */
const Type *fact_adjusted_type_of(const Facts *facts, const ASTExpr *expr);
const Type *fact_return_type_of(const Facts *facts, const ASTStmt *stmt);

/* The local a place expression ultimately reads, or NULL where it does not name one. */
Binding *fact_root_local(const Facts *facts, const ASTExpr *expr);

#endif
