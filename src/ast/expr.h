#ifndef GAB_AST_EXPR_H
#define GAB_AST_EXPR_H

#include "symbol_table.h"
#include "type.h"

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
    };

    Type *type;     // Filled during type resolution
    Symbol *symbol; // Filled during symbol resolution
} ASTExpr;

ASTExpr *ast_literal_expr_create(Literal value);
ASTExpr *ast_bin_op_expr_create(ASTExpr *left, BinOp op, ASTExpr *right);
ASTExpr *ast_variable_expr_create(StringRef name);
void ast_expr_free(ASTExpr *node);

#endif
