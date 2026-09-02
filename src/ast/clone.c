#include "ast/clone.h"

#include "ast/expr.h"
#include "ast/type_expr.h"

#include <assert.h>

static ASTExpr *clone_expr(Arena *arena, const ASTExpr *expr);
static ASTStmt *clone_stmt(Arena *arena, const ASTStmt *stmt);

static ASTField *clone_field(Arena *arena, const ASTField *field) {
    return ast_field_create(arena, field->span, field->name, field->type_expr);
}

static ASTExprList clone_expr_list(Arena *arena, const ASTExprList *list) {
    ASTExprList out = ast_expr_list_create(arena_allocator(arena));

    for (size_t i = 0; i < list->size; i++) {
        ast_expr_list_add(&out, clone_expr(arena, list->data[i]));
    }

    return out;
}

static ASTExpr *clone_expr(Arena *arena, const ASTExpr *expr) {
    if (!expr) {
        return NULL;
    }

    switch (expr->kind) {
    case EXPR_LITERAL:
        return ast_literal_expr_create(arena, expr->span, expr->lit);

    case EXPR_BIN_OP:
        return ast_bin_op_expr_create(arena, expr->span, clone_expr(arena, expr->bin_op.left),
                                      expr->bin_op.op, clone_expr(arena, expr->bin_op.right));

    case EXPR_VARIABLE:
        return ast_variable_expr_create(arena, expr->span, expr->var.name);

    case EXPR_CALL:
        return ast_call_expr_create(arena, expr->span, clone_expr(arena, expr->call.target),
                                    clone_expr_list(arena, &expr->call.args));

    case EXPR_FIELD:
        return ast_field_expr_create(arena, expr->span, clone_expr(arena, expr->field.target),
                                     expr->field.name);

    case EXPR_ADDR_OF:
        return ast_addr_of_expr_create(arena, expr->span, clone_expr(arena, expr->unary.target));

    case EXPR_DEREF:
        return ast_deref_expr_create(arena, expr->span, clone_expr(arena, expr->unary.target));

    case EXPR_LEND:
        return ast_lend_expr_create(arena, expr->span, clone_expr(arena, expr->lend.target));

    case EXPR_NEG:
        return ast_neg_expr_create(arena, expr->span, clone_expr(arena, expr->unary.target));

    case EXPR_NOT:
        return ast_not_expr_create(arena, expr->span, clone_expr(arena, expr->unary.target));

    case EXPR_CAST:
        return ast_cast_expr_create(arena, expr->span, clone_expr(arena, expr->cast.operand));

    case EXPR_BOX:
        return ast_box_expr_create(arena, expr->span, clone_expr(arena, expr->box_expr.value));

    case EXPR_INDEX:
        return ast_index_expr_create(arena, expr->span, clone_expr(arena, expr->index.target),
                                     clone_expr(arena, expr->index.index));

    case EXPR_ARRAY_LIT:
        return ast_array_lit_expr_create(arena, expr->span,
                                         clone_expr_list(arena, &expr->array_lit.elements));

    case EXPR_STRUCT_LIT: {
        ASTFieldInitList fields = ast_field_init_list_create(arena_allocator(arena));

        for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
            const ASTFieldInit *field = &expr->struct_lit.fields.data[i];

            ast_field_init_list_add(&fields, (ASTFieldInit){.name = field->name,
                                                            .value = clone_expr(arena, field->value),
                                                            .span = field->span});
        }

        return ast_struct_lit_expr_create(arena, expr->span, expr->struct_lit.type_expr, fields);
    }
    }

    assert(false && "an expression is one of the kinds above");

    return NULL;
}

static ASTStmt *clone_stmt(Arena *arena, const ASTStmt *stmt) {
    if (!stmt) {
        return NULL;
    }

    switch (stmt->kind) {
    case STMT_EXPR:
        return ast_expr_stmt_create(arena, stmt->span, clone_expr(arena, stmt->expr.value));

    case STMT_VAR_DECL:
        return ast_var_decl_stmt_create(arena, stmt->span, stmt->var_decl.name, stmt->var_decl.type_expr,
                                        clone_expr(arena, stmt->var_decl.initializer));

    case STMT_IMPL:
        assert(false && "a generic instantiation clones a member, not its impl block");

        return NULL;

    case STMT_FUNC_DECL: {
        ASTFieldList params = ast_field_list_create(arena_allocator(arena));

        for (size_t i = 0; i < stmt->func_decl.params.size; i++) {
            ast_field_list_add(&params, clone_field(arena, stmt->func_decl.params.data[i]));
        }

        ASTStmt *clone =
            ast_func_decl_stmt_create(arena, stmt->span, stmt->func_decl.name, stmt->func_decl.return_type,
                                      params, clone_stmt(arena, stmt->func_decl.body));

        clone->func_decl.owner = stmt->func_decl.owner;

        clone->func_decl.type_param_count = stmt->func_decl.type_param_count;

        for (size_t i = 0; i < stmt->func_decl.type_param_count; i++) {
            clone->func_decl.type_params[i] = stmt->func_decl.type_params[i];
        }

        return clone;
    }

    case STMT_ASSIGN:
        return ast_assign_stmt_create(arena, stmt->span, clone_expr(arena, stmt->assign.target),
                                      clone_expr(arena, stmt->assign.value));

    case STMT_COMPOUND_ASSIGN:
        return ast_compound_assign_stmt_create(
            arena, stmt->span, clone_expr(arena, stmt->compound_assign.target), stmt->compound_assign.op,
            clone_expr(arena, stmt->compound_assign.value));

    case STMT_BLOCK: {
        ASTStmtList list = ast_stmt_list_create(arena_allocator(arena));

        for (size_t i = 0; i < stmt->block.list.size; i++) {
            ast_stmt_list_add(&list, clone_stmt(arena, stmt->block.list.data[i]));
        }

        return ast_block_stmt_create(arena, stmt->span, list);
    }

    case STMT_IF:
        return ast_if_stmt_create(arena, stmt->span, clone_expr(arena, stmt->ifstmt.condition),
                                  clone_stmt(arena, stmt->ifstmt.then_block),
                                  clone_stmt(arena, stmt->ifstmt.else_block));

    case STMT_FOR:
        return ast_for_stmt_create(arena, stmt->span, clone_stmt(arena, stmt->forstmt.init),
                                   clone_expr(arena, stmt->forstmt.condition),
                                   clone_stmt(arena, stmt->forstmt.post),
                                   clone_stmt(arena, stmt->forstmt.body));

    case STMT_JUMP:
        return ast_jump_stmt_create(arena, stmt->span, stmt->jump.is_break);

    case STMT_RETURN:
        return ast_return_stmt_create(arena, stmt->span, clone_expr(arena, stmt->ret.result));

    case STMT_STRUCT_DECL:
        break;
    }

    assert(false && "a statement in a function body is one of the kinds above");

    return NULL;
}

ASTStmt *ast_clone_stmt(Arena *arena, const ASTStmt *stmt) { return clone_stmt(arena, stmt); }
