#ifndef GAB_AST_H
#define GAB_AST_H

#include "symbol_table.h"
#include "type.h"
#include "type_registry.h"
#include "string/string.h"
#include "string/string_ref.h"

#include <stddef.h>
#include <stdlib.h>

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

    Type *type;    // Filled during type resolution
    Symbol symbol; // Filled during symbol resolution
} ASTExpr;

ASTExpr *ast_literal_expr_create(Literal value);
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
} StmtKind;

typedef struct ASTStmt {
    StmtKind kind;

    union {
        struct {
            ASTExpr *value;
        } expr;

        struct {
            StringRef name;
            TypeSpec *type_spec;
            ASTExpr *initializer;

            Symbol symbol; // Filled during symbol/type resolution
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
ASTStmt *ast_var_decl_stmt_create(StringRef name, TypeSpec *type, ASTExpr *initializer);
ASTStmt *ast_assign_stmt_create(ASTExpr *target, ASTExpr *value);
ASTStmt *ast_if_stmt_create(ASTExpr *condition, ASTStmt *then_block, ASTStmt *else_block);
ASTStmt *ast_block_stmt_create(ASTStmtList list);
ASTStmt *ast_return_stmt_create(ASTExpr *result);
void ast_stmt_free(ASTStmt *stmt);

typedef struct ASTScript {
    SymbolTable *symbol_table;
    TypeRegistry *type_registry;

    ASTStmtList statements;
    int vars_count;
} ASTScript;

ASTScript *ast_script_create();
void ast_script_add_statement(ASTScript *script, ASTStmt *stmt);
void ast_script_resolve(ASTScript *script);
void ast_script_free(ASTScript *script);

#endif
