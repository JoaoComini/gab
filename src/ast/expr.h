#ifndef GAB_AST_EXPR_H
#define GAB_AST_EXPR_H

#include "ast/type_expr.h"
#include "decl.h"
#include "diagnostics.h"
#include "memory/arena.h"
#include "string/string.h"
#include "type/type.h"
#include "util/list.h"

#include <stdbool.h>
#include <stdint.h>

/* What the source spelled, before any type exists to describe it; the parser knows this much and the
 * resolver turns it into a type. Only these four can be written, which a 'TypeKind' would not say. */
typedef enum {
    LITERAL_INT,
    LITERAL_FLOAT,
    LITERAL_BOOL,
    LITERAL_STRING,
} LiteralKind;

typedef struct {
    LiteralKind kind;
    union {
        int64_t as_int;
        float as_float;
        bool as_bool;

        String *as_string;
    };
} Literal;

/* The type a literal of this kind has, which is what the resolver gives the expression holding it. */
TypeKind literal_type_kind(LiteralKind kind);

typedef enum {
    EXPR_LITERAL,
    EXPR_BIN_OP,
    EXPR_VARIABLE,
    EXPR_CALL,
    EXPR_FIELD,
    EXPR_ADDR_OF,
    EXPR_DEREF,

    EXPR_NEG,
    EXPR_NOT,
    EXPR_BOX,

    /* A value the compiler supplies, named by the lexeme past '@'. */
    EXPR_BUILTIN,

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
            StringRef name;

            /* What '@size_of<T>()' measures; none for a builtin that takes no type. */
            TypeExpr *type_expr;
        } builtin;

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
        } field;

        struct {
            ASTExpr *target;
        } unary;

        struct {
            ASTExpr *value;
        } box_expr;
    };

    Span span;
} ASTExpr;

ASTExpr *ast_literal_expr_create(Arena *arena, Span span, Literal value);
ASTExpr *ast_bin_op_expr_create(Arena *arena, Span span, ASTExpr *left, BinOp op, ASTExpr *right);
ASTExpr *ast_variable_expr_create(Arena *arena, Span span, StringRef name);
ASTExpr *ast_builtin_expr_create(Arena *arena, Span span, StringRef name, TypeExpr *type_expr);
ASTExpr *ast_call_expr_create(Arena *arena, Span span, ASTExpr *target, ASTExprList args);
ASTExpr *ast_field_expr_create(Arena *arena, Span span, ASTExpr *target, StringRef name);
ASTExpr *ast_addr_of_expr_create(Arena *arena, Span span, ASTExpr *target);
ASTExpr *ast_deref_expr_create(Arena *arena, Span span, ASTExpr *target);
ASTExpr *ast_neg_expr_create(Arena *arena, Span span, ASTExpr *target);
ASTExpr *ast_not_expr_create(Arena *arena, Span span, ASTExpr *target);
ASTExpr *ast_box_expr_create(Arena *arena, Span span, ASTExpr *value);
ASTExpr *ast_array_lit_expr_create(Arena *arena, Span span, ASTExprList elements);
ASTExpr *ast_struct_lit_expr_create(Arena *arena, Span span, TypeExpr *type_expr, ASTFieldInitList fields);
ASTExpr *ast_index_expr_create(Arena *arena, Span span, ASTExpr *target, ASTExpr *index);

#endif
