#ifndef GAB_AST_TYPE_EXPR_H
#define GAB_AST_TYPE_EXPR_H

#include "arena.h"
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

    StringRef name;

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

TypeExpr *type_expr_name(Arena *arena, StringRef name);
TypeExpr *type_expr_indirect(Arena *arena, TypeExprKind kind, TypeExpr *inner);
TypeExpr *type_expr_apply(Arena *arena, TypeExpr *base);
TypeExpr *type_expr_const(Arena *arena, int32_t value);

#endif
