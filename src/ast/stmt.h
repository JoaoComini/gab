#ifndef GAB_AST_STMT_H
#define GAB_AST_STMT_H

#include "ast/expr.h"
#include "string/string_ref.h"
#include "symbol_table.h"
#include "type.h"
#include "util/list.h"

typedef struct ASTField {
    StringRef name;
    TypeSpec *type_spec;

    Symbol *symbol; // Filled during symbol/type resolution
} ASTField;

ASTField *ast_field_create(StringRef name, TypeSpec *type_spec);
void ast_field_destroy(ASTField *field);

#define ast_field_list_item_free ast_field_destroy
GAB_LIST(ASTFieldList, ast_field_list, ASTField *);

typedef struct ASTStmt ASTStmt;
void ast_stmt_destroy(ASTStmt *stmt);

#define ast_stmt_list_item_free ast_stmt_destroy
GAB_LIST(ASTStmtList, ast_stmt_list, ASTStmt *);

typedef enum {
    STMT_EXPR,
    STMT_VAR_DECL,
    STMT_FUNC_DECL,
    STMT_ASSIGN,
    STMT_BLOCK,
    STMT_IF,
    STMT_RETURN,
} StmtKind;

typedef struct {
    ASTExpr *value;
} ASTExprStmt;

typedef struct {
    StringRef name;
    TypeSpec *type_spec;
    ASTExpr *initializer;

    Symbol *symbol; // Filled during symbol/type resolution
} ASTVarDecl;

typedef struct {
    StringRef name;
    TypeSpec *return_type;
    ASTFieldList params;
    struct ASTStmt *body;

    Symbol *symbol;
} ASTFuncDecl;

typedef struct {
    ASTExpr *target;
    ASTExpr *value;
} ASTAssignStmt;

typedef struct {
    ASTExpr *condition;
    struct ASTStmt *then_block;
    struct ASTStmt *else_block;
} ASTIfStmt;

typedef struct {
    ASTStmtList list;
} ASTBlockStmt;

typedef struct {
    ASTExpr *result;
} ASTReturnStmt;

typedef struct ASTStmt {
    StmtKind kind;

    union {
        ASTExprStmt expr;
        ASTVarDecl var_decl;
        ASTFuncDecl func_decl;
        ASTAssignStmt assign;
        ASTIfStmt ifstmt;
        ASTBlockStmt block;
        ASTReturnStmt ret;
    };

} ASTStmt;

ASTStmt *ast_expr_stmt_create(ASTExpr *value);
ASTStmt *ast_var_decl_stmt_create(StringRef name, TypeSpec *type, ASTExpr *initializer);
ASTStmt *ast_func_decl_stmt_create(StringRef name, TypeSpec *return_type, ASTFieldList params, ASTStmt *body);
ASTStmt *ast_assign_stmt_create(ASTExpr *target, ASTExpr *value);
ASTStmt *ast_if_stmt_create(ASTExpr *condition, ASTStmt *then_block, ASTStmt *else_block);
ASTStmt *ast_block_stmt_create(ASTStmtList list);
ASTStmt *ast_return_stmt_create(ASTExpr *result);

#endif
