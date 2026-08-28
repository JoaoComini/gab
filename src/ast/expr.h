#ifndef GAB_AST_EXPR_H
#define GAB_AST_EXPR_H

#include "diagnostics.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type.h"
#include "util/list.h"

#include <stdint.h>

typedef struct {
    TypeKind kind;
    union {
        int32_t as_int;
        float as_float;

        // The characters the literal denotes, decoded and interned by the lexer.
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

    // A value handing over a reference to what it already holds: an owning
    // string giving the address and count of its characters.
    //
    // A node rather than nothing, though the two are the same bytes today: what
    // the reference carries is read out of the header by field offset, so a
    // header that grows a field the reference does not carry still lends the
    // two words it does. Without it the lend is a reinterpretation that holds
    // only while the layouts agree, and nothing says they must.
    EXPR_LEND,
    EXPR_NEG,
    EXPR_NOT,
    EXPR_CAST,
    EXPR_NEW,

    // 'xs[i]' -- one element of an array, and '[T; N]', which allocates
    // one. Distinguished by which of the two fields is set.
    EXPR_INDEX,

    // '[a, b, c]'. The elements of a fixed array, written out.
    EXPR_ARRAY_LIT,
    EXPR_MOVE,
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

#define ast_expr_list_item_free(item) (void)(item)
GAB_LIST(ASTExprList, ast_expr_list, ASTExpr *)

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
        } var;

        // 'f(args)', and 'recv.m(args)' before it resolves: a method call is a
        // call whose target is the field expression 'recv.m'. Resolution tells
        // the two apart, and rewrites a method call into an ordinary one with
        // the receiver as argument zero — which is what parameter zero has
        // always been.
        //
        // 'target' is what was called, and is only read during resolution:
        // afterwards the callee is on 'symbol' and the arguments are complete,
        // so codegen never looks at it.
        struct {
            ASTExpr *target;
            ASTExprList args;
        } call;

        // 'xs[index]'. Assignable and addressable like a field, so it resolves
        // to the element type and codegen reaches it the same way -- an offset
        // from an address -- except that the offset is computed rather than
        // known.
        // The elements of an array literal, in the order written. How many
        // there are is checked against the length the destination's type says.
        struct {
            ASTExprList elements;
        } array_lit;

        struct {
            ASTExpr *target;
            ASTExpr *index;

            // The array the target reaches, once any indirection is stripped.
            // Its element is what the expression yields.
            const Type *array_type;
        } index;

        struct {
            ASTExpr *target;
            StringRef name;

            // The struct the field was found on, and which of its fields it
            // is. Resolved during type resolution so codegen need not look the
            // name up again.
            //
            // A position rather than a pointer to the field: where a field
            // begins is a layout question, so it is the pair that codegen asks
            // the registry with. What the field is called and what type it has
            // are read back through ast_field_of, which is the same walk a
            // 'getelementptr' or a MIR field projection describes.
            const Type *owner;
            size_t index;
        } field;

        // 'ref target', '*target', '-target' and '!target'. All are prefix forms
        // over a single operand, so they share a shape.
        struct {
            ASTExpr *target;
        } unary;

        // 'int(x)' — a numeric conversion, parsed as a call whose target names
        // a type rather than a function. The resolver rewrites it into this,
        // so codegen never sees a call it would have to tell apart.
        struct {
            ASTExpr *operand;
        } cast;

        // 'new T' — a heap allocation yielding an owned 'box T'. Names a type
        // rather than taking an operand, so it carries a TypeExpr the way a
        // declaration does rather than an expression.
        struct {
            TypeExpr *type_expr;

            // The struct being allocated, resolved from the spec. The
            // expression's own type is the pointer to it.
            const Type *type;
        } new_expr;
    };

    Span span; // Source position, for diagnostics

    const Type *type; // Filled during type resolution
    Symbol *symbol;   // Filled during symbol resolution
} ASTExpr;

ASTExpr *ast_literal_expr_create(Span span, Literal value);
ASTExpr *ast_bin_op_expr_create(Span span, ASTExpr *left, BinOp op, ASTExpr *right);
ASTExpr *ast_variable_expr_create(Span span, StringRef name);
ASTExpr *ast_call_expr_create(Span span, ASTExpr *target, ASTExprList args);
ASTExpr *ast_field_expr_create(Span span, ASTExpr *target, StringRef name);
ASTExpr *ast_addr_of_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_deref_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_lend_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_move_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_neg_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_not_expr_create(Span span, ASTExpr *target);
ASTExpr *ast_cast_expr_create(Span span, ASTExpr *operand);
ASTExpr *ast_new_expr_create(Span span, TypeExpr *type_expr);
ASTExpr *ast_array_lit_expr_create(Span span, ASTExprList elements);
ASTExpr *ast_index_expr_create(Span span, ASTExpr *target, ASTExpr *index);

// The field a resolved field access names, or NULL before resolution has found
// one. What it is called and what type it has, read back from the position the
// resolver settled -- where it begins is the layout's answer, not this one's.
const TypeField *ast_field_of(const ASTExpr *expr);
void ast_expr_free(ASTExpr *node);

#endif
