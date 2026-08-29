#include "stmt.h"

ASTStmt *ast_stmt_create(Span span) {
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    stmt->span = span;

    return stmt;
}

ASTStmt *ast_expr_stmt_create(Span span, ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_EXPR;
    stmt->expr.value = value;
    return stmt;
}

ASTStmt *ast_var_decl_stmt_create(Span span, StringRef name, TypeExpr *type_expr, ASTExpr *initializer) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_VAR_DECL;
    stmt->var_decl.name = name;
    stmt->var_decl.type_expr = type_expr;
    stmt->var_decl.initializer = initializer;
    stmt->var_decl.symbol = NULL;
    return stmt;
}

ASTStmt *ast_func_decl_stmt_create(Span span, StringRef name, TypeExpr *return_type, ASTFieldList params,
                                   ASTStmt *body) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_FUNC_DECL;
    stmt->func_decl.name = name;
    stmt->func_decl.owner = NULL;
    stmt->func_decl.return_type = return_type;
    stmt->func_decl.params = params;
    stmt->func_decl.body = body;
    stmt->func_decl.symbol = NULL;
    stmt->func_decl.resolved_return_type = NULL;
    stmt->func_decl.declared = false;
    return stmt;
}

ASTStmt *ast_struct_decl_stmt_create(Span span, StringRef name, const StringRef *params, size_t param_count,
                                     ASTFieldList fields) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_STRUCT_DECL;
    stmt->struct_decl.name = name;
    stmt->struct_decl.fields = fields;
    stmt->struct_decl.param_count = param_count;

    for (size_t i = 0; i < param_count; i++) {
        stmt->struct_decl.params[i] = params[i];
    }

    stmt->struct_decl.type = NULL;
    stmt->struct_decl.declared = false;
    return stmt;
}

ASTStmt *ast_assign_stmt_create(Span span, ASTExpr *target, ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_ASSIGN;
    stmt->assign.target = target;
    stmt->assign.value = value;
    return stmt;
}

ASTStmt *ast_compound_assign_stmt_create(Span span, ASTExpr *target, BinOp op, ASTExpr *value) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_COMPOUND_ASSIGN;
    stmt->compound_assign.target = target;
    stmt->compound_assign.value = value;
    stmt->compound_assign.op = op;
    return stmt;
}

ASTStmt *ast_if_stmt_create(Span span, ASTExpr *condition, ASTStmt *then_block, ASTStmt *else_block) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_IF;
    stmt->ifstmt.condition = condition;
    stmt->ifstmt.then_block = then_block;
    stmt->ifstmt.else_block = else_block;
    return stmt;
}

ASTStmt *ast_for_stmt_create(Span span, ASTStmt *init, ASTExpr *condition, ASTStmt *post, ASTStmt *body) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_FOR;
    stmt->forstmt.init = init;
    stmt->forstmt.condition = condition;
    stmt->forstmt.post = post;
    stmt->forstmt.body = body;
    stmt->forstmt.scope = NULL;
    return stmt;
}

ASTStmt *ast_jump_stmt_create(Span span, bool is_break) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_JUMP;
    stmt->jump.is_break = is_break;
    return stmt;
}

ASTStmt *ast_block_stmt_create(Span span, ASTStmtList list) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_BLOCK;
    stmt->block.list = list;
    stmt->block.scope = NULL;
    return stmt;
}

ASTStmt *ast_return_stmt_create(Span span, ASTExpr *result) {
    ASTStmt *stmt = ast_stmt_create(span);
    stmt->kind = STMT_RETURN;
    stmt->ret.result = result;
    return stmt;
}

void ast_stmt_destroy(ASTStmt *stmt) {
    if (!stmt)
        return;

    switch (stmt->kind) {
    case STMT_EXPR:
        ast_expr_free(stmt->expr.value);
        break;
    case STMT_VAR_DECL:
        ast_expr_free(stmt->var_decl.initializer);
        if (stmt->var_decl.type_expr) {
            type_expr_destroy(stmt->var_decl.type_expr);
        }
        break;
    case STMT_FUNC_DECL:
        if (stmt->func_decl.return_type) {
            type_expr_destroy(stmt->func_decl.return_type);
        }
        if (stmt->func_decl.owner) {
            type_expr_destroy(stmt->func_decl.owner);
        }

        ast_field_list_free(&stmt->func_decl.params);
        ast_stmt_destroy(stmt->func_decl.body);
        break;
    case STMT_STRUCT_DECL:
        ast_field_list_free(&stmt->struct_decl.fields);
        break;
    case STMT_ASSIGN:
        ast_expr_free(stmt->assign.target);
        ast_expr_free(stmt->assign.value);
        break;
    case STMT_COMPOUND_ASSIGN:
        ast_expr_free(stmt->compound_assign.target);
        ast_expr_free(stmt->compound_assign.value);
        break;
    case STMT_IF:
        ast_expr_free(stmt->ifstmt.condition);
        ast_stmt_destroy(stmt->ifstmt.then_block);
        ast_stmt_destroy(stmt->ifstmt.else_block);
        break;
    case STMT_FOR:
        ast_stmt_destroy(stmt->forstmt.init);
        ast_expr_free(stmt->forstmt.condition);
        ast_stmt_destroy(stmt->forstmt.post);
        ast_stmt_destroy(stmt->forstmt.body);
        break;
    case STMT_JUMP:
        break;
    case STMT_BLOCK:
        ast_stmt_list_free(&stmt->block.list);
        break;
    case STMT_RETURN:
        ast_expr_free(stmt->ret.result);
        break;
    }

    free(stmt);
}

ASTField *ast_field_create(Span span, StringRef name, TypeExpr *type_expr) {
    ASTField *field = malloc(sizeof(ASTField));
    field->span = span;
    field->name = name;
    field->type_expr = type_expr;
    field->symbol = NULL;

    return field;
}

void ast_field_destroy(ASTField *field) {
    // NULL-tolerant, since an absent receiver is a NULL field and every error
    // path in the function parser frees one whether or not it was there.
    if (!field) {
        return;
    }

    type_expr_destroy(field->type_expr);
    free(field);
}
