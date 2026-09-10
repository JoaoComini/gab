#ifndef GAB_AST_TYPE_EXPR_H
#define GAB_AST_TYPE_EXPR_H

#include "ast/ident.h"
#include "memory/arena.h"
#include "string/string_ref.h"
#include "util/list.h"

#include <stdint.h>

typedef struct TypeExpr TypeExpr;

GAB_LIST(TypeExprList, type_expr_list, TypeExpr *)

typedef enum {
    TYPE_EXPR_NAME,

    TYPE_EXPR_BOX,
    TYPE_EXPR_REF,

    TYPE_EXPR_CONST,

    TYPE_EXPR_APPLY,
} TypeExprKind;

struct TypeExpr {
    TypeExprKind kind;

    ASTIdent *name;

    /* The module a written name is qualified by, or null where it names one directly. */
    ASTIdent *qualifier;

    union {
        struct {
            TypeExpr *inner;
        } indirect;

        struct {
            TypeExpr *base;
            TypeExprList args;
        } apply;

        int32_t constant;
    };
};

TypeExpr *type_expr_name(Arena *arena, ASTIdent *name);

/* 'Module::Name' as a written type, whose halves the source spelled apart. */
TypeExpr *type_expr_qualified(Arena *arena, ASTIdent *qualifier, ASTIdent *name);
TypeExpr *type_expr_indirect(Arena *arena, TypeExprKind kind, TypeExpr *inner);
TypeExpr *type_expr_apply(Arena *arena, TypeExpr *base);
TypeExpr *type_expr_const(Arena *arena, int32_t value);

#endif
