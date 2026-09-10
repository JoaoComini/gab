#ifndef GAB_AST_FACTS_H
#define GAB_AST_FACTS_H

#include "ast/expr.h"
#include "ast/stmt.h"
#include "constant.h"
#include "decl.h"
#include "memory/arena.h"
#include "type/type.h"
#include "util/hash_map.h"

#define expr_fact_hash(key) ((size_t)(key) >> 4)
#define expr_fact_key_equals(key, other) ((key) == (other))

#define stmt_fact_hash(key) expr_fact_hash(key)
#define stmt_fact_key_equals(key, other) ((key) == (other))

#define def_fact_hash(key) expr_fact_hash(key)
#define def_fact_key_equals(key, other) ((key) == (other))

typedef enum {
    CALL_FUNCTION,
    CALL_CONVERSION,

    CALL_METHOD,

    CALL_INDEX,
} CallKind;

typedef enum {
    ADJUST_NONE,
    ADJUST_BORROW,
    ADJUST_UNSIZE,
} AdjustKind;

typedef struct {
    AdjustKind kind;

    unsigned int derefs;

    const Type **deref_types;

    const Type *to;

    union {
        int32_t length;
    };
} Adjustment;

typedef struct {
    const Type *type;

    Symbol *use;

    Function *callee;

    Adjustment adjustment;

    Constant constant;

    size_t field;

    size_t initialized_field;

    CallKind call;

    bool moves;

    bool has_constant;
} ExprFact;

typedef struct {
    Function *function;
} StmtFact;

GAB_HASH_MAP(ExprFactMap, expr_fact, const ASTExpr *, ExprFact)
GAB_HASH_MAP(StmtFactMap, stmt_fact, const ASTStmt *, StmtFact)

GAB_HASH_MAP(DefMap, def_fact, const ASTIdent *, Symbol *)

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
void fact_set_function(Facts *facts, const ASTStmt *stmt, Function *function);

void fact_set_def(Facts *facts, const ASTIdent *name, Symbol *binding);

const ExprFact *fact_of(const Facts *facts, const ASTExpr *expr);

const Type *fact_type_of(const Facts *facts, const ASTExpr *expr);
Symbol *fact_use_of(const Facts *facts, const ASTExpr *expr);
Function *fact_callee_of(const Facts *facts, const ASTExpr *expr);
bool fact_moves(const Facts *facts, const ASTExpr *expr);

bool fact_constant_of(const Facts *facts, const ASTExpr *expr, Constant *out);

Adjustment fact_adjustment(const Facts *facts, const ASTExpr *expr);

CallKind fact_call_kind(const Facts *facts, const ASTExpr *expr);

size_t fact_field_of(const Facts *facts, const ASTExpr *expr);

size_t fact_initialized_field_of(const Facts *facts, const ASTExpr *value);

const Type *fact_adjusted_type_of(const Facts *facts, const ASTExpr *expr);

Function *fact_function_of(const Facts *facts, const ASTStmt *stmt);

Symbol *fact_def_of(const Facts *facts, const ASTIdent *name);

Symbol *fact_root_local(const Facts *facts, const ASTExpr *expr);

#endif
