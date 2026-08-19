#ifndef GAB_AST_EXPR_H
#define GAB_AST_EXPR_H

#include "diagnostics.h"
#include "symbol_table.h"
#include "type.h"
#include "util/list.h"

#include <stdint.h>

typedef struct {
    TypeKind kind;
    union {
        int32_t as_int;
        float as_float;
    };
} Literal;

typedef enum {
    EXPR_LITERAL,
    EXPR_BIN_OP,
    EXPR_VARIABLE,
    EXPR_CALL,
    EXPR_METHOD_CALL,
    EXPR_FIELD,
    EXPR_ADDR_OF,
    EXPR_DEREF,
    EXPR_NEW,
} ExprKind;

typedef enum {
    BIN_OP_ADD,
    BIN_OP_SUB,
    BIN_OP_MUL,
    BIN_OP_DIV,
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

        struct {
            ASTExpr *target;
            ASTExprList args;
        } call;

        // 'recv.name(args)'. The receiver is kept apart from the arguments
        // rather than prepended to them: the resolver would otherwise have to
        // synthesize an '&recv' node over a target it has already walked, and
        // there is no later lowering pass to host that rewrite. Codegen puts
        // the receiver in the first argument slot, which is where the callee's
        // parameter zero already expects it.
        struct {
            ASTExpr *receiver;
            StringRef name;
            ASTExprList args;

            // Resolved during type resolution. The method is an ordinary
            // function symbol, so codegen emits the OP_CALL it already would.
            Symbol *method;

            // How the receiver reaches parameter zero. A '*T' method called on
            // a 'T' takes its address; a 'T' method called through a '*T'
            // copies the pointee in. At most one is ever set.
            bool take_address;
            bool deref;
        } method_call;

        struct {
            ASTExpr *target;
            StringRef name;

            // Resolved during type resolution so codegen need not look it up
            // again.
            const TypeField *field;
        } field;

        // '&target' and '*target'. Both are prefix forms over a single
        // operand, so they share a shape.
        struct {
            ASTExpr *target;
        } unary;

        // 'new T' — a heap allocation yielding an owned '*T'. Names a type
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
ASTExpr *ast_method_call_expr_create(Span span, ASTExpr *receiver, StringRef name, ASTExprList args);
ASTExpr *ast_field_expr_create(Span span, ASTExpr *target, StringRef name);
ASTExpr *ast_addr_of_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_deref_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_new_expr_create(Span span, TypeSpec *type_spec);
void ast_expr_free(ASTExpr *node);

#endif
