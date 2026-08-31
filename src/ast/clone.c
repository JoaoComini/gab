#include "ast/clone.h"

#include "ast/expr.h"
#include "ast/type_expr.h"

#include <assert.h>
#include <stdlib.h>

static TypeExpr *clone_type_expr(const TypeExpr *expr);
static ASTExpr *clone_expr(const ASTExpr *expr);
static ASTStmt *clone_stmt(const ASTStmt *stmt);

static TypeExpr *clone_type_expr(const TypeExpr *expr) {
    if (!expr) {
        return NULL;
    }

    switch (expr->kind) {
    case TYPE_EXPR_NAME:
        return type_expr_name(expr->name);

    case TYPE_EXPR_BOX:
    case TYPE_EXPR_REF:
        return type_expr_indirect(expr->kind, clone_type_expr(expr->indirect.inner));

    case TYPE_EXPR_ARRAY:
        return type_expr_array(clone_type_expr(expr->array.element), expr->array.length);

    case TYPE_EXPR_APPLY: {
        TypeExpr *apply = type_expr_apply(clone_type_expr(expr->apply.base));

        for (size_t i = 0; i < expr->apply.args.size; i++) {
            type_expr_list_add(&apply->apply.args, clone_type_expr(expr->apply.args.data[i]));
        }

        return apply;
    }
    }

    assert(false && "a type expression is one of the kinds above");

    return NULL;
}

static ASTField *clone_field(const ASTField *field) {
    return ast_field_create(field->span, field->name, clone_type_expr(field->type_expr));
}

static ASTExprList clone_expr_list(const ASTExprList *list) {
    ASTExprList out = ast_expr_list_create();

    for (size_t i = 0; i < list->size; i++) {
        ast_expr_list_add(&out, clone_expr(list->data[i]));
    }

    return out;
}

static ASTExpr *clone_expr(const ASTExpr *expr) {
    if (!expr) {
        return NULL;
    }

    switch (expr->kind) {
    case EXPR_LITERAL:
        return ast_literal_expr_create(expr->span, expr->lit);

    case EXPR_BIN_OP:
        return ast_bin_op_expr_create(expr->span, clone_expr(expr->bin_op.left), expr->bin_op.op,
                                      clone_expr(expr->bin_op.right));

    case EXPR_VARIABLE:
        return ast_variable_expr_create(expr->span, expr->var.name);

    case EXPR_CALL:
        return ast_call_expr_create(expr->span, clone_expr(expr->call.target),
                                    clone_expr_list(&expr->call.args));

    case EXPR_FIELD:
        return ast_field_expr_create(expr->span, clone_expr(expr->field.target), expr->field.name);

    case EXPR_ADDR_OF:
        return ast_addr_of_expr_create(expr->span, clone_expr(expr->unary.target));

    case EXPR_DEREF:
        return ast_deref_expr_create(expr->span, clone_expr(expr->unary.target));

    case EXPR_LEND:
        return ast_lend_expr_create(expr->span, clone_expr(expr->lend.target));

    case EXPR_NEG:
        return ast_neg_expr_create(expr->span, clone_expr(expr->unary.target));

    case EXPR_NOT:
        return ast_not_expr_create(expr->span, clone_expr(expr->unary.target));

    case EXPR_CAST:
        return ast_cast_expr_create(expr->span, clone_expr(expr->cast.operand));

    case EXPR_BOX:
        return ast_box_expr_create(expr->span, clone_expr(expr->box_expr.value));

    case EXPR_INDEX:
        return ast_index_expr_create(expr->span, clone_expr(expr->index.target),
                                     clone_expr(expr->index.index));

    case EXPR_ARRAY_LIT:
        return ast_array_lit_expr_create(expr->span, clone_expr_list(&expr->array_lit.elements));

    case EXPR_STRUCT_LIT: {
        ASTFieldInitList fields = ast_field_init_list_create();

        for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
            const ASTFieldInit *field = &expr->struct_lit.fields.data[i];

            ast_field_init_list_add(
                &fields,
                (ASTFieldInit){.name = field->name, .value = clone_expr(field->value), .span = field->span});
        }

        return ast_struct_lit_expr_create(expr->span, clone_type_expr(expr->struct_lit.type_expr), fields);
    }
    }

    assert(false && "an expression is one of the kinds above");

    return NULL;
}

static ASTStmt *clone_stmt(const ASTStmt *stmt) {
    if (!stmt) {
        return NULL;
    }

    switch (stmt->kind) {
    case STMT_EXPR:
        return ast_expr_stmt_create(stmt->span, clone_expr(stmt->expr.value));

    case STMT_VAR_DECL:
        return ast_var_decl_stmt_create(stmt->span, stmt->var_decl.name,
                                        clone_type_expr(stmt->var_decl.type_expr),
                                        clone_expr(stmt->var_decl.initializer));

    case STMT_FUNC_DECL: {
        ASTFieldList params = ast_field_list_create();

        for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
            ast_field_list_add(&params, clone_field(stmt->func_decl.params.data[i]));
        }

        ASTStmt *clone = ast_func_decl_stmt_create(stmt->span, stmt->func_decl.name,
                                                   clone_type_expr(stmt->func_decl.return_type), params,
                                                   clone_stmt(stmt->func_decl.body));

        clone->func_decl.owner = clone_type_expr(stmt->func_decl.owner);

        clone->func_decl.type_param_count = stmt->func_decl.type_param_count;

        for (size_t i = 0; i < stmt->func_decl.type_param_count; i++) {
            clone->func_decl.type_params[i] = stmt->func_decl.type_params[i];
        }

        return clone;
    }

    case STMT_ASSIGN:
        return ast_assign_stmt_create(stmt->span, clone_expr(stmt->assign.target),
                                      clone_expr(stmt->assign.value));

    case STMT_COMPOUND_ASSIGN:
        return ast_compound_assign_stmt_create(stmt->span, clone_expr(stmt->compound_assign.target),
                                               stmt->compound_assign.op,
                                               clone_expr(stmt->compound_assign.value));

    case STMT_BLOCK: {
        ASTStmtList list = ast_stmt_list_create();

        for (size_t i = 0; i < stmt->block.list.size; i++) {
            ast_stmt_list_add(&list, clone_stmt(stmt->block.list.data[i]));
        }

        return ast_block_stmt_create(stmt->span, list);
    }

    case STMT_IF:
        return ast_if_stmt_create(stmt->span, clone_expr(stmt->ifstmt.condition),
                                  clone_stmt(stmt->ifstmt.then_block), clone_stmt(stmt->ifstmt.else_block));

    case STMT_FOR:
        return ast_for_stmt_create(stmt->span, clone_stmt(stmt->forstmt.init),
                                   clone_expr(stmt->forstmt.condition), clone_stmt(stmt->forstmt.post),
                                   clone_stmt(stmt->forstmt.body));

    case STMT_JUMP:
        return ast_jump_stmt_create(stmt->span, stmt->jump.is_break);

    case STMT_RETURN:
        return ast_return_stmt_create(stmt->span, clone_expr(stmt->ret.result));

    case STMT_STRUCT_DECL:
        break;
    }

    assert(false && "a statement in a function body is one of the kinds above");

    return NULL;
}

ASTStmt *ast_clone_stmt(const ASTStmt *stmt) { return clone_stmt(stmt); }
