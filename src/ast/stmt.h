#ifndef GAB_AST_STMT_H
#define GAB_AST_STMT_H

#include "arena.h"
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

ASTField *ast_field_create(Arena *arena, Span span, StringRef name, TypeExpr *type_expr);

GAB_LIST(ASTFieldList, ast_field_list, ASTField *);

typedef struct ASTStmt ASTStmt;

GAB_LIST(ASTStmtList, ast_stmt_list, ASTStmt *);

typedef enum {
    STMT_EXPR,
    STMT_VAR_DECL,
    STMT_FUNC_DECL,
    STMT_STRUCT_DECL,
    STMT_IMPL,
    STMT_INTERFACE_DECL,
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

    StringRef type_params[GAB_MAX_TYPE_PARAMS];

    /* The interface each type parameter is bounded by, null where it is unbounded. */
    TypeExpr *type_param_bounds[GAB_MAX_TYPE_PARAMS];
    size_t type_param_count;

    Function *function;

    const Type *resolved_return_type;

    /* Set when the declaration is 'intrinsic', so no body is written and none is bound. */
    bool is_intrinsic;

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
    StringRef name;
    ASTStmtList members;

    StringRef params[GAB_MAX_TYPE_PARAMS];
    size_t param_count;
} ASTInterfaceDecl;

typedef struct {
    TypeExpr *type;
    ASTStmtList members;

    StringRef interface_name;
    Span interface_span;

    /* The arguments the 'as' clause applies to the interface, empty where it names none. */
    TypeExprList interface_args;

    /* The bound written on each parameter the block declares, which says whether it takes a value. */
    TypeExpr *param_bounds[GAB_MAX_TYPE_PARAMS];
} ASTImplStmt;

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
        ASTImplStmt impl;
        ASTInterfaceDecl interface_decl;
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

ASTStmt *ast_expr_stmt_create(Arena *arena, Span span, ASTExpr *value);
ASTStmt *ast_var_decl_stmt_create(Arena *arena, Span span, StringRef name, TypeExpr *type,
                                  ASTExpr *initializer);
ASTStmt *ast_func_decl_stmt_create(Arena *arena, Span span, StringRef name, TypeExpr *return_type,
                                   ASTFieldList params, ASTStmt *body);
ASTStmt *ast_struct_decl_stmt_create(Arena *arena, Span span, StringRef name, const StringRef *params,
                                     size_t param_count, ASTFieldList fields);
ASTStmt *ast_assign_stmt_create(Arena *arena, Span span, ASTExpr *target, ASTExpr *value);
ASTStmt *ast_compound_assign_stmt_create(Arena *arena, Span span, ASTExpr *target, BinOp op, ASTExpr *value);
ASTStmt *ast_if_stmt_create(Arena *arena, Span span, ASTExpr *condition, ASTStmt *then_block,
                            ASTStmt *else_block);
ASTStmt *ast_for_stmt_create(Arena *arena, Span span, ASTStmt *init, ASTExpr *condition, ASTStmt *post,
                             ASTStmt *body);
ASTStmt *ast_jump_stmt_create(Arena *arena, Span span, bool is_break);
ASTStmt *ast_impl_stmt_create(Arena *arena, Span span, TypeExpr *type, ASTStmtList members);
ASTStmt *ast_interface_decl_stmt_create(Arena *arena, Span span, StringRef name, ASTStmtList members);
ASTStmt *ast_block_stmt_create(Arena *arena, Span span, ASTStmtList list);
ASTStmt *ast_return_stmt_create(Arena *arena, Span span, ASTExpr *result);

#endif
