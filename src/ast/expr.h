#ifndef GAB_AST_EXPR_H
#define GAB_AST_EXPR_H

#include "diagnostics.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type.h"
#include "util/list.h"

#include <stdint.h>

typedef struct {
    TypeKind kind;
    union {
        int32_t as_int;
        float as_float;

        // The characters the literal denotes, decoded and interned by the lexer.
        String *as_string;
    };
} Literal;

typedef enum {
    EXPR_LITERAL,
    EXPR_BIN_OP,
    EXPR_VARIABLE,
    EXPR_CALL,
    EXPR_FIELD,
    EXPR_ADDR_OF,
    EXPR_DEREF,
    EXPR_NEG,
    EXPR_NOT,
    EXPR_CAST,
    EXPR_NEW,
    EXPR_MOVE,
} ExprKind;

typedef enum {
    BIN_OP_ADD,
    BIN_OP_SUB,
    BIN_OP_MUL,
    BIN_OP_DIV,
    BIN_OP_MOD,
    BIN_OP_LESS,
    BIN_OP_GREATER,
    BIN_OP_EQUAL,
    BIN_OP_NEQUAL,
    BIN_OP_LEQUAL,
    BIN_OP_GEQUAL,
    BIN_OP_AND,
    BIN_OP_OR,
} BinOp;

typedef struct ASTExpr ASTExpr;

#define ast_expr_list_item_free(item) (void)(item)
GAB_LIST(ASTExprList, ast_expr_list, ASTExpr *)

typedef struct ASTExpr {
    ExprKind kind;

    union {
        Literal lit;

        struct {
            struct ASTExpr *left;
            struct ASTExpr *right;
            BinOp op;
        } bin_op;

        struct {
            StringRef name;
        } var;

        // 'f(args)', and 'recv.m(args)' before it resolves: a method call is a
        // call whose target is the field expression 'recv.m'. Resolution tells
        // the two apart, and rewrites a method call into an ordinary one with
        // the receiver as argument zero — which is what parameter zero has
        // always been.
        //
        // 'target' is what was called, and is only read during resolution:
        // afterwards the callee is on 'symbol' and the arguments are complete,
        // so codegen never looks at it.
        struct {
            ASTExpr *target;
            ASTExprList args;
        } call;

        struct {
            ASTExpr *target;
            StringRef name;

            // Resolved during type resolution so codegen need not look it up
            // again.
            const TypeField *field;
        } field;

        // 'ref target', '*target', '-target' and '!target'. All are prefix forms
        // over a single operand, so they share a shape.
        struct {
            ASTExpr *target;
        } unary;

        // 'int(x)' — a numeric conversion, parsed as a call whose target names
        // a type rather than a function. The resolver rewrites it into this,
        // so codegen never sees a call it would have to tell apart.
        struct {
            ASTExpr *operand;
        } cast;

        // 'new T' — a heap allocation yielding an owned 'box T'. Names a type
        // rather than taking an operand, so it carries a TypeSpec the way a
        // declaration does rather than an expression.
        struct {
            TypeSpec *type_spec;

            // The struct being allocated, resolved from the spec. The
            // expression's own type is the pointer to it.
            Type *type;
        } new_expr;
    };

    Span span; // Source position, for diagnostics

    Type *type;     // Filled during type resolution
    Symbol *symbol; // Filled during symbol resolution
} ASTExpr;

ASTExpr *ast_literal_expr_create(Span span, Literal value);
ASTExpr *ast_bin_op_expr_create(Span span, ASTExpr *left, BinOp op, ASTExpr *right);
ASTExpr *ast_variable_expr_create(Span span, StringRef name);
ASTExpr *ast_call_expr_create(Span span, ASTExpr *target, ASTExprList args);
ASTExpr *ast_field_expr_create(Span span, ASTExpr *target, StringRef name);
ASTExpr *ast_addr_of_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_deref_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_move_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_neg_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_not_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_cast_expr_create(Span span, ASTExpr *operand);
ASTExpr *ast_new_expr_create(Span span, TypeSpec *type_spec);
void ast_expr_free(ASTExpr *node);

#endif
