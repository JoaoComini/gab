#ifndef GAB_AST_TYPE_EXPR_H
#define GAB_AST_TYPE_EXPR_H

// A type as the source wrote it. Syntax rather than a resolved type, so it
// belongs beside the rest of the AST: the resolver is what turns one into the
// interned Type it names.

#include "string/string_ref.h"
#include "util/list.h"

#include <stdint.h>

// A type as the source wrote it, before any name is looked up. The syntactic
// counterpart of Type: this is what a type position parses into, and the
// resolver evaluates it to the interned Type it names.
//
// A tree rather than a name plus a count of indirections, because the
// constructors nest freely and one of them takes arguments. Anything flatter
// needs a field per constructor that does not fit -- which is what an array's
// element was -- and a width to bound the nesting, which is a limit on what a
// program may say rather than on anything real.
typedef struct TypeExpr TypeExpr;

void type_expr_destroy(TypeExpr *expr);

#define type_expr_list_item_free(item) type_expr_destroy(item)
GAB_LIST(TypeExprList, type_expr_list, TypeExpr *)

typedef enum {
    // A name, possibly qualified: 'int', 'Player', 'Module::Type'. The leaf
    // every other kind bottoms out in.
    TYPE_EXPR_NAME,

    // 'box T' and 'ref T', each wrapping the one level it spells.
    TYPE_EXPR_BOX,
    TYPE_EXPR_REF,

    // A constructor applied to arguments: 'Vec<int>' today, and whatever takes
    // more than one later. A list rather than a single argument, so that a
    // second one needs no second field.
    TYPE_EXPR_APPLY,
} TypeExprKind;

struct TypeExpr {
    TypeExprKind kind;

    // The name, for TYPE_EXPR_NAME. A qualified name is kept as one ref over
    // the source, so the resolver sees it exactly as the registry stores it.
    StringRef name;

    union {
        // What a 'box' or a 'ref' wraps.
        struct {
            TypeExpr *inner;
        } indirect;

        struct {
            TypeExpr *base;
            TypeExprList args;

            // How many elements, for '[T; N]'. An integer literal rather
            // than an argument of its own: a length is not a type, and the one
            // constructor that takes one always takes exactly one.
            int32_t length;
        } apply;
    };
};

TypeExpr *type_expr_name(StringRef name);
TypeExpr *type_expr_indirect(TypeExprKind kind, TypeExpr *inner);
TypeExpr *type_expr_apply(TypeExpr *base);

#endif
