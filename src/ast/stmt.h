#ifndef GAB_AST_STMT_H
#define GAB_AST_STMT_H

#include "ast/expr.h"
#include "ast/type_expr.h"
#include "memory/arena.h"
#include "scope.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "type/type.h"
#include "util/list.h"

typedef struct ASTField {
    ASTIdent *name;
    TypeExpr *type_expr;
} ASTField;

ASTField *ast_field_create(Arena *arena, ASTIdent *name, TypeExpr *type_expr);

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
    ASTIdent *name;
    TypeExpr *type_expr;
    ASTExpr *initializer;
} ASTVarDecl;

/* What was written before 'func'. Syntax, not a conclusion: what these mean for a symbol is decided
 * once the resolver can also see whether a body follows. */
typedef enum {
    FUNC_SYN_NONE = 0,

    FUNC_SYN_INTRINSIC = 1 << 0,
    FUNC_SYN_EXTERN = 1 << 1,
    FUNC_SYN_FOREIGN = 1 << 2,
    FUNC_SYN_CALLER = 1 << 3,
} FuncSyntax;

typedef struct {
    ASTIdent *name;

    TypeExpr *owner;

    TypeExpr *return_type;
    ASTFieldList params;
    struct ASTStmt *body;

    ASTIdent *type_params[GAB_MAX_TYPE_PARAMS];

    /* The interface each type parameter is bounded by, null where it is unbounded. */
    TypeExpr *type_param_bounds[GAB_MAX_TYPE_PARAMS];
    size_t type_param_count;

    /* A set of FuncSyntax. */
    unsigned syntax;
} ASTFuncDecl;

typedef struct {
    ASTIdent *name;
    ASTFieldList fields;

    ASTIdent *params[GAB_MAX_TYPE_PARAMS];
    size_t param_count;

    /* The compiler supplies what this type means, which only a name it knows may claim. */
    bool intrinsic;
} ASTStructDecl;

typedef struct {
    ASTIdent *name;
    ASTStmtList members;

    ASTIdent *params[GAB_MAX_TYPE_PARAMS];
    size_t param_count;
} ASTInterfaceDecl;

typedef struct {
    TypeExpr *type;
    ASTStmtList members;

    ASTIdent *interface_name;

    /* The arguments the 'as' clause applies to the interface, empty where it names none. */
    TypeExprList interface_args;

    /* The bound written on each parameter the block declares, which says whether it takes a value. */
    TypeExpr *param_bounds[GAB_MAX_TYPE_PARAMS];

    ASTIdent *param_names[GAB_MAX_TYPE_PARAMS];
    size_t param_count;
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
} ASTForStmt;

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
ASTStmt *ast_var_decl_stmt_create(Arena *arena, Span span, ASTIdent *name, TypeExpr *type,
                                  ASTExpr *initializer);
ASTStmt *ast_func_decl_stmt_create(Arena *arena, Span span, ASTIdent *name, TypeExpr *return_type,
                                   ASTFieldList params, ASTStmt *body);
ASTStmt *ast_struct_decl_stmt_create(Arena *arena, Span span, ASTIdent *name, ASTIdent *const *params,
                                     size_t param_count, ASTFieldList fields, bool intrinsic);
ASTStmt *ast_assign_stmt_create(Arena *arena, Span span, ASTExpr *target, ASTExpr *value);
ASTStmt *ast_compound_assign_stmt_create(Arena *arena, Span span, ASTExpr *target, BinOp op, ASTExpr *value);
ASTStmt *ast_if_stmt_create(Arena *arena, Span span, ASTExpr *condition, ASTStmt *then_block,
                            ASTStmt *else_block);
ASTStmt *ast_for_stmt_create(Arena *arena, Span span, ASTStmt *init, ASTExpr *condition, ASTStmt *post,
                             ASTStmt *body);
ASTStmt *ast_jump_stmt_create(Arena *arena, Span span, bool is_break);
ASTStmt *ast_impl_stmt_create(Arena *arena, Span span, TypeExpr *type, ASTStmtList members);
ASTStmt *ast_interface_decl_stmt_create(Arena *arena, Span span, ASTIdent *name, ASTStmtList members);
ASTStmt *ast_block_stmt_create(Arena *arena, Span span, ASTStmtList list);
ASTStmt *ast_return_stmt_create(Arena *arena, Span span, ASTExpr *result);

#endif
