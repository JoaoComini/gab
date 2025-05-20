#ifndef GAB_AST_H
#define GAB_AST_H

#include "string_ref.h"
#include "symbol_table.h"
#include "variant.h"
#include <stddef.h>
#include <stdlib.h>

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
    BIN_OP_LESS,
    BIN_OP_GREATER,
    BIN_OP_EQUAL,
    BIN_OP_NEQUAL,
    BIN_OP_LEQUAL,
    BIN_OP_GEQUAL,
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
            StringRef name;
        } variable;
    };

    Symbol symbol; // Filled during symbol resolution
} ASTExpr;

ASTExpr *ast_literal_expr_create(Variant value);
ASTExpr *ast_bin_op_expr_create(ASTExpr *left, BinOp op, ASTExpr *right);
ASTExpr *ast_variable_expr_create(StringRef name);
void ast_expr_free(ASTExpr *node);

typedef struct ASTStmt ASTStmt;

typedef struct ASTStmtList {
    ASTStmt **data;
    size_t size;
} ASTStmtList;

ASTStmtList ast_stmt_list_create();
void ast_stmt_list_add(ASTStmtList *list, ASTStmt *stmt);
void ast_stmt_list_free(ASTStmtList list);

typedef enum {
    STMT_EXPR,
    STMT_VAR_DECL,
    STMT_ASSIGN,
    STMT_BLOCK,
    STMT_IF,
    STMT_RETURN,
} StmtType;

typedef struct ASTStmt {
    StmtType type;

    union {
        struct {
            ASTExpr *value;
        } expr;

        struct {
            StringRef name;
            ASTExpr *initializer;

            int reg; // Filled during symbol resolution
        } var_decl;

        struct {
            ASTExpr *target;
            ASTExpr *value;
        } assign;

        struct {
            ASTExpr *condition;
            struct ASTStmt *then_block;
            struct ASTStmt *else_block;
        } ifstmt;

        struct {
            ASTStmtList list;
        } block;

        struct {
            ASTExpr *result;
        } ret;
    };

} ASTStmt;

ASTStmt *ast_expr_stmt_create(ASTExpr *value);
ASTStmt *ast_var_decl_stmt_create(StringRef name, ASTExpr *initializer);
ASTStmt *ast_assign_stmt_create(ASTExpr *target, ASTExpr *value);
ASTStmt *ast_if_stmt_create(ASTExpr *condition, ASTStmt *then_block, ASTStmt *else_block);
ASTStmt *ast_block_stmt_create(ASTStmtList list);
ASTStmt *ast_return_stmt_create(ASTExpr *result);
void ast_stmt_free(ASTStmt *stmt);

typedef struct ASTScript {
    SymbolTable *symbol_table;

    ASTStmtList statements;
    int vars_count;
} ASTScript;

ASTScript *ast_script_create();
void ast_script_add_statement(ASTScript *script, ASTStmt *stmt);
void ast_script_resolve_symbols(ASTScript *script);
void ast_script_free(ASTScript *script);

#endif
