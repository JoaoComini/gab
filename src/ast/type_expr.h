#ifndef GAB_AST_TYPE_EXPR_H
#define GAB_AST_TYPE_EXPR_H

#include "string/string_ref.h"
#include "util/list.h"

#include <stdint.h>

typedef struct TypeExpr TypeExpr;

void type_expr_destroy(TypeExpr *expr);

#define type_expr_list_item_free(item) type_expr_destroy(item)
GAB_LIST(TypeExprList, type_expr_list, TypeExpr *)

typedef enum {
    TYPE_EXPR_NAME,

    TYPE_EXPR_BOX,
    TYPE_EXPR_REF,

    TYPE_EXPR_ARRAY,

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

        struct {
            TypeExpr *element;
            int32_t length;
        } array;
    };
};

TypeExpr *type_expr_name(StringRef name);
TypeExpr *type_expr_indirect(TypeExprKind kind, TypeExpr *inner);
TypeExpr *type_expr_apply(TypeExpr *base);
TypeExpr *type_expr_array(TypeExpr *element, int32_t length);

#endif
