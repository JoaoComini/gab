#ifndef GAB_AST_EXPR_H
#define GAB_AST_EXPR_H

#include "arena.h"
#include "ast/type_expr.h"
#include "binding.h"
#include "diagnostics.h"
#include "string/string.h"
#include "type/type.h"
#include "util/list.h"

#include <stdint.h>

typedef struct {
    TypeKind kind;
    union {
        int32_t as_int;
        float as_float;

        String *as_string;
    };
} Literal;

typedef enum {
    EXPR_LITERAL,
    EXPR_BIN_OP,
    EXPR_VARIABLE,
    EXPR_CALL,
    EXPR_FIELD,
    EXPR_ADDR_OF,
    EXPR_DEREF,

    EXPR_LEND,
    EXPR_UNSIZE,
    EXPR_NEG,
    EXPR_NOT,
    EXPR_CAST,
    EXPR_BOX,

    EXPR_INDEX,

    EXPR_ARRAY_LIT,
    EXPR_STRUCT_LIT,
} ExprKind;

typedef enum {
    BIN_OP_ADD,
    BIN_OP_SUB,
    BIN_OP_MUL,
    BIN_OP_DIV,
    BIN_OP_MOD,

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

GAB_LIST(ASTExprList, ast_expr_list, ASTExpr *)

typedef struct {
    StringRef name;
    ASTExpr *value;
    Span span;

    size_t index;
} ASTFieldInit;

GAB_LIST(ASTFieldInitList, ast_field_init_list, ASTFieldInit)

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

            TypeExpr *owner_type_expr;
        } var;

        struct {
            ASTExpr *target;
            ASTExprList args;
        } call;

        struct {
            ASTExprList elements;
        } array_lit;

        struct {
            TypeExpr *type_expr;
            ASTFieldInitList fields;
        } struct_lit;

        struct {
            ASTExpr *target;
            ASTExpr *index;
        } index;

        struct {
            ASTExpr *target;
            StringRef name;

            const Type *owner;
            size_t index;
        } field;

        struct {
            ASTExpr *target;
        } unary;

        struct {
            ASTExpr *target;

            const LentPart *parts;
            size_t part_count;
        } lend;

        struct {
            ASTExpr *target;
            int32_t length;
        } unsize;

        struct {
            ASTExpr *operand;
        } cast;

        struct {
            ASTExpr *value;

            const Type *type;
        } box_expr;
    };

    Span span;

    const Type *type;

    Binding *binding;
    Function *callee;

    bool moves;
} ASTExpr;

ASTExpr *ast_literal_expr_create(Arena *arena, Span span, Literal value);
ASTExpr *ast_bin_op_expr_create(Arena *arena, Span span, ASTExpr *left, BinOp op, ASTExpr *right);
ASTExpr *ast_variable_expr_create(Arena *arena, Span span, StringRef name);
ASTExpr *ast_call_expr_create(Arena *arena, Span span, ASTExpr *target, ASTExprList args);
ASTExpr *ast_field_expr_create(Arena *arena, Span span, ASTExpr *target, StringRef name);
ASTExpr *ast_addr_of_expr_create(Arena *arena, Span span, ASTExpr *target);
ASTExpr *ast_deref_expr_create(Arena *arena, Span span, ASTExpr *target);
ASTExpr *ast_lend_expr_create(Arena *arena, Span span, ASTExpr *target);
ASTExpr *ast_unsize_expr_create(Arena *arena, Span span, ASTExpr *target, int32_t length);
ASTExpr *ast_neg_expr_create(Arena *arena, Span span, ASTExpr *target);
ASTExpr *ast_not_expr_create(Arena *arena, Span span, ASTExpr *target);
ASTExpr *ast_cast_expr_create(Arena *arena, Span span, ASTExpr *operand);
ASTExpr *ast_box_expr_create(Arena *arena, Span span, ASTExpr *value);
ASTExpr *ast_array_lit_expr_create(Arena *arena, Span span, ASTExprList elements);
ASTExpr *ast_struct_lit_expr_create(Arena *arena, Span span, TypeExpr *type_expr, ASTFieldInitList fields);
ASTExpr *ast_index_expr_create(Arena *arena, Span span, ASTExpr *target, ASTExpr *index);

const TypeField *ast_field_of(TypeRegistry *registry, const ASTExpr *expr);

Binding *ast_root_local(const ASTExpr *expr);

Binding *ast_binding_of(const ASTExpr *expr);
void ast_bind(ASTExpr *expr, Binding *binding);

#endif
