#ifndef GAB_AST_STMT_H
#define GAB_AST_STMT_H

#include "ast/expr.h"
#include "ast/type_expr.h"
#include "binding.h"
#include "string/string_ref.h"
#include "type/type.h"
#include "util/list.h"

typedef struct ASTField {
    StringRef name;
    TypeExpr *type_expr;

    Span span;

    Binding *binding;
} ASTField;

ASTField *ast_field_create(Span span, StringRef name, TypeExpr *type_expr);
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
    STMT_STRUCT_DECL,
    STMT_ASSIGN,
    STMT_COMPOUND_ASSIGN,
    STMT_BLOCK,
    STMT_IF,
    STMT_FOR,
    STMT_JUMP,
    STMT_RETURN,
} StmtKind;

typedef struct {
    ASTExpr *value;
} ASTExprStmt;

typedef struct {
    StringRef name;
    TypeExpr *type_expr;
    ASTExpr *initializer;

    Binding *binding;
} ASTVarDecl;

typedef struct {
    StringRef name;

    TypeExpr *owner;

    TypeExpr *return_type;
    ASTFieldList params;
    struct ASTStmt *body;

    Binding *binding;

    const Type *resolved_return_type;

    bool declared;
} ASTFuncDecl;

typedef struct {
    StringRef name;
    ASTFieldList fields;

    StringRef params[GAB_MAX_TYPE_PARAMS];
    size_t param_count;

    bool declared;
} ASTStructDecl;

typedef struct {
    ASTExpr *target;
    ASTExpr *value;
} ASTAssignStmt;

typedef struct {
    ASTExpr *target;
    ASTExpr *value;
    BinOp op;
} ASTCompoundAssignStmt;

typedef struct {
    ASTExpr *condition;
    struct ASTStmt *then_block;
    struct ASTStmt *else_block;
} ASTIfStmt;

typedef struct {
    struct ASTStmt *init;
    ASTExpr *condition;
    struct ASTStmt *post;
    struct ASTStmt *body;

    struct Scope *scope;
} ASTForStmt;

typedef struct {
    bool is_break;
} ASTJumpStmt;

typedef struct {
    ASTStmtList list;

    struct Scope *scope;
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
        ASTStructDecl struct_decl;
        ASTAssignStmt assign;
        ASTCompoundAssignStmt compound_assign;
        ASTIfStmt ifstmt;
        ASTForStmt forstmt;
        ASTJumpStmt jump;
        ASTBlockStmt block;
        ASTReturnStmt ret;
    };

    Span span;
} ASTStmt;

ASTStmt *ast_expr_stmt_create(Span span, ASTExpr *value);
ASTStmt *ast_var_decl_stmt_create(Span span, StringRef name, TypeExpr *type, ASTExpr *initializer);
ASTStmt *ast_func_decl_stmt_create(Span span, StringRef name, TypeExpr *return_type, ASTFieldList params,
                                   ASTStmt *body);
ASTStmt *ast_struct_decl_stmt_create(Span span, StringRef name, const StringRef *params, size_t param_count,
                                     ASTFieldList fields);
ASTStmt *ast_assign_stmt_create(Span span, ASTExpr *target, ASTExpr *value);
ASTStmt *ast_compound_assign_stmt_create(Span span, ASTExpr *target, BinOp op, ASTExpr *value);
ASTStmt *ast_if_stmt_create(Span span, ASTExpr *condition, ASTStmt *then_block, ASTStmt *else_block);
ASTStmt *ast_for_stmt_create(Span span, ASTStmt *init, ASTExpr *condition, ASTStmt *post, ASTStmt *body);
ASTStmt *ast_jump_stmt_create(Span span, bool is_break);
ASTStmt *ast_block_stmt_create(Span span, ASTStmtList list);
ASTStmt *ast_return_stmt_create(Span span, ASTExpr *result);

#endif
