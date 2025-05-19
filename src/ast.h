#ifndef GAB_AST_H
#define GAB_AST_H

#include "symbol_table.h"
#include "variant.h"
#include <stddef.h>

typedef enum {
    EXPR_LITERAL,
    EXPR_BIN_OP,
    EXPR_VARIABLE,
} ExprType;

typedef enum {
    BIN_OP_ADD,
    BIN_OP_SUB,
    BIN_OP_MUL,
    BIN_OP_DIV,
} BinOp;

typedef struct ASTExpr {
    ExprType type;
    union {
        struct {
            Variant value;
        } literal;

        struct {
            struct ASTExpr *left;
            struct ASTExpr *right;
            BinOp op;
        } bin_op;

        struct {
            char *name;
        } variable;
    };

    Symbol symbol; // Filled during symbol resolution
} ASTExpr;

ASTExpr *ast_literal_expr_create(Variant value);
ASTExpr *ast_bin_op_expr_create(ASTExpr *left, BinOp op, ASTExpr *right);
ASTExpr *ast_variable_expr_create(char *name);
void ast_expr_free(ASTExpr *node);

typedef enum {
    STMT_EXPR,
    STMT_VAR_DECL,
    STMT_ASSIGN,
    STMT_RETURN,
} StmtType;

typedef struct {
    StmtType type;

    union {
        struct {
            ASTExpr *value;
        } expr;

        struct {
            char *name;
            ASTExpr *initializer;

            int reg; // Filled during symbol resolution
        } var_decl;

        struct {
            ASTExpr *target;
            ASTExpr *value;
        } assign;

        struct {
            ASTExpr *result;
        } ret;
    };

} ASTStmt;

ASTStmt *ast_expr_stmt_create(ASTExpr *value);
ASTStmt *ast_var_decl_stmt_create(char *name, ASTExpr *initializer);
ASTStmt *ast_assign_stmt_create(ASTExpr *target, ASTExpr *value);
ASTStmt *ast_return_stmt_create(ASTExpr *result);
void ast_stmt_free(ASTStmt *stmt);

typedef struct ASTScript {
    SymbolTable *symbol_table;

    ASTStmt **statements;
    size_t statements_size;

    int vars_count;
} ASTScript;

ASTScript *ast_script_create();
void ast_script_add_statement(ASTScript *script, ASTStmt *stmt);
void ast_script_resolve_symbols(ASTScript *script);
void ast_script_free(ASTScript *script);

#endif
