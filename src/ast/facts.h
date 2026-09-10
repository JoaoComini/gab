#ifndef GAB_AST_FACTS_H
#define GAB_AST_FACTS_H

#include "ast/expr.h"
#include "ast/stmt.h"
#include "constant.h"
#include "decl.h"
#include "memory/arena.h"
#include "type/type.h"
#include "util/hash_map.h"

/* A node is one object for the whole of a unit, so its address names it. */
#define expr_fact_hash(key) ((size_t)(key) >> 4)
#define expr_fact_key_equals(key, other) ((key) == (other))

#define stmt_fact_hash(key) expr_fact_hash(key)
#define stmt_fact_key_equals(key, other) ((key) == (other))

#define def_fact_hash(key) expr_fact_hash(key)
#define def_fact_key_equals(key, other) ((key) == (other))

/* What a call names, which its target's resolution decides: a function to call, or a type to convert to. */
typedef enum {
    CALL_FUNCTION,
    CALL_CONVERSION,

    /* Written as 'a.f(x)', which resolution answers with the receiver standing as the first argument. */
    CALL_METHOD,

    /* Written as 'xs[i]', which resolution answers with the call the element's 'Index' names. */
    CALL_INDEX,
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

/* Everything resolution concluded about one expression. One record rather than a map per question,
 * so a stage asking two of them pays for one lookup. */
typedef struct {
    const Type *type;

    /* What a name denotes, where it denotes a binding. */
    Symbol *use;

    /* The function a call names, which a specialization has already been applied to. */
    Function *callee;

    Adjustment adjustment;

    /* What the expression was found to be worth, where resolution could answer it outright. */
    Constant constant;

    /* Which field of its struct a field access names. */
    size_t field;

    /* Which field of its struct an initializer fills. An initializer's value is a node of its own,
     * so it is kept apart from 'field': the same node is both where it names one and fills another. */
    size_t initialized_field;

    CallKind call;

    bool moves;

    bool has_constant;
} ExprFact;

/* Everything resolution concluded about one statement, held as one record for the same reason an
 * expression's is. */
typedef struct {
    /* The type the function this statement declares was declared to give back. */
    const Type *return_type;

    /* The function a declaration declares. */
    Function *function;
} StmtFact;

GAB_HASH_MAP(ExprFactMap, expr_fact, const ASTExpr *, ExprFact)
GAB_HASH_MAP(StmtFactMap, stmt_fact, const ASTStmt *, StmtFact)

/* What a name the source declares was bound to. Keyed on the name rather than what contains it, so
 * a parameter and a variable are one kind of answer. */
GAB_HASH_MAP(DefMap, def_fact, const ASTIdent *, Symbol *)

/* What resolution concluded about each node, which only what it hands on can read. */
typedef struct Facts {
    ExprFactMap exprs;

    StmtFactMap stmts;

    DefMap defs;
} Facts;

void facts_init(Facts *facts, Arena *arena);

void fact_set_type(Facts *facts, const ASTExpr *expr, const Type *type);
void fact_set_use(Facts *facts, const ASTExpr *expr, Symbol *binding);
void fact_set_callee(Facts *facts, const ASTExpr *expr, Function *callee);
void fact_set_moves(Facts *facts, const ASTExpr *expr, bool moves);
void fact_set_adjustment(Facts *facts, const ASTExpr *expr, Adjustment adjustment);
void fact_set_call_kind(Facts *facts, const ASTExpr *expr, CallKind kind);
void fact_set_constant(Facts *facts, const ASTExpr *expr, Constant constant);
void fact_set_field(Facts *facts, const ASTExpr *expr, size_t field);
void fact_set_initialized_field(Facts *facts, const ASTExpr *value, size_t field);
void fact_set_return_type(Facts *facts, const ASTStmt *stmt, const Type *type);
void fact_set_function(Facts *facts, const ASTStmt *stmt, Function *function);

/* Records what a declared name was bound to, which is what a use of it resolves to. */
void fact_set_def(Facts *facts, const ASTIdent *name, Symbol *binding);

/* Everything concluded about one expression, or nothing where resolution reached none of it. */
const ExprFact *fact_of(const Facts *facts, const ASTExpr *expr);

const Type *fact_type_of(const Facts *facts, const ASTExpr *expr);
Symbol *fact_use_of(const Facts *facts, const ASTExpr *expr);
Function *fact_callee_of(const Facts *facts, const ASTExpr *expr);
bool fact_moves(const Facts *facts, const ASTExpr *expr);

/* Whether resolution answered this expression with a constant, which lowering emits rather than the node. */
bool fact_constant_of(const Facts *facts, const ASTExpr *expr, Constant *out);

/* The coercion a value needs where it sits; its kind is ADJUST_NONE where it needs none. */
Adjustment fact_adjustment(const Facts *facts, const ASTExpr *expr);

/* What a call names; a call resolution rejected never reaches a stage that asks. */
CallKind fact_call_kind(const Facts *facts, const ASTExpr *expr);

/* Which field of its struct a field access names. */
size_t fact_field_of(const Facts *facts, const ASTExpr *expr);

/* Which field of its struct an initializer fills. */
size_t fact_initialized_field_of(const Facts *facts, const ASTExpr *value);

/* The type a value has once its coercion is applied, which is its own where it has none. */
const Type *fact_adjusted_type_of(const Facts *facts, const ASTExpr *expr);
const Type *fact_return_type_of(const Facts *facts, const ASTStmt *stmt);

/* The function a declaration declares, or null where it declared none. */
Function *fact_function_of(const Facts *facts, const ASTStmt *stmt);

/* What a declared name was bound to, or null where nothing was. */
Symbol *fact_def_of(const Facts *facts, const ASTIdent *name);

/* The local a place expression ultimately reads, or NULL where it does not name one. */
Symbol *fact_root_local(const Facts *facts, const ASTExpr *expr);

#endif
