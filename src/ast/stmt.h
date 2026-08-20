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

    Span span; // Source position, for diagnostics

    Symbol *symbol; // Filled during symbol/type resolution
} ASTField;

ASTField *ast_field_create(Span span, StringRef name, TypeSpec *type_spec);
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
    TypeSpec *type_spec;
    ASTExpr *initializer;

    Symbol *symbol; // Filled during symbol/type resolution
} ASTVarDecl;

typedef struct {
    StringRef name;

    // 'func (p: *Player) damage(...)' — NULL for a free function. A receiver is
    // a parameter in every respect, so it reuses ASTField, and the resolver
    // makes it parameter zero of the symbol it declares.
    ASTField *receiver;

    TypeSpec *return_type;
    ASTFieldList params;
    struct ASTStmt *body;

    Symbol *symbol;

    // Filled by the resolver's declaration pass. The return type is held here
    // and not only on the Symbol because a duplicate name leaves no Symbol,
    // and the body still has to be checked against what it declared.
    Type *resolved_return_type;

    // Whether the declaration pass has already run over this. A nested
    // function, which nothing above it could have seen, is declared by the
    // body walk instead.
    bool declared;
} ASTFuncDecl;

typedef struct {
    StringRef name;
    ASTFieldList fields;

    Type *type;

    // As ASTFuncDecl::declared.
    bool declared;
} ASTStructDecl;

typedef struct {
    ASTExpr *target;
    ASTExpr *value;
} ASTAssignStmt;

typedef struct {
    ASTExpr *condition;
    struct ASTStmt *then_block;
    struct ASTStmt *else_block;
} ASTIfStmt;

// Every loop form is this one node. 'for { }' leaves all three clauses NULL,
// 'for cond { }' fills only the condition, and the three-clause form fills what
// it was given -- so an omitted condition means the same thing in each: loop
// forever.
typedef struct {
    struct ASTStmt *init;
    ASTExpr *condition;
    struct ASTStmt *post;
    struct ASTStmt *body;
} ASTForStmt;

// 'break' and 'continue'. They differ only in which end of the loop they jump
// to, so they share a node rather than duplicating one.
typedef struct {
    bool is_break;
} ASTJumpStmt;

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
        ASTStructDecl struct_decl;
        ASTAssignStmt assign;
        ASTIfStmt ifstmt;
        ASTForStmt forstmt;
        ASTJumpStmt jump;
        ASTBlockStmt block;
        ASTReturnStmt ret;
    };

    Span span; // Source position, for diagnostics
} ASTStmt;

ASTStmt *ast_expr_stmt_create(Span span, ASTExpr *value);
ASTStmt *ast_var_decl_stmt_create(Span span, StringRef name, TypeSpec *type, ASTExpr *initializer);
ASTStmt *ast_func_decl_stmt_create(Span span, StringRef name, ASTField *receiver, TypeSpec *return_type,
                                   ASTFieldList params, ASTStmt *body);
ASTStmt *ast_struct_decl_stmt_create(Span span, StringRef name, ASTFieldList fields);
ASTStmt *ast_assign_stmt_create(Span span, ASTExpr *target, ASTExpr *value);
ASTStmt *ast_if_stmt_create(Span span, ASTExpr *condition, ASTStmt *then_block, ASTStmt *else_block);
ASTStmt *ast_for_stmt_create(Span span, ASTStmt *init, ASTExpr *condition, ASTStmt *post, ASTStmt *body);
ASTStmt *ast_jump_stmt_create(Span span, bool is_break);
ASTStmt *ast_block_stmt_create(Span span, ASTStmtList list);
ASTStmt *ast_return_stmt_create(Span span, ASTExpr *result);

#endif
