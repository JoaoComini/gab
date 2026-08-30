#include "ast/flow_pass.h"

#include "ast/cfg.h"
#include "ast/expr.h"
#include "ast/flow.h"
#include "type/type.h"

#include <string.h>

typedef struct {
    Flow *flow;
    Arena *arena;
    Diagnostics *diagnostics;

    const Type *return_type;

    bool reporting;

    bool assigning_field;

    bool assigning;
} FlowPass;

static void flow_pass_expr(FlowPass *pass, ASTExpr *expr);

static void flow_report(FlowPass *pass, Span span, const char *format, const char *arg) {
    if (!pass->reporting) {
        return;
    }

    diag_error(pass->diagnostics, GAB_ERR_LIFETIME, span, format, arg);
}

static int inner_depth(FlowPass *pass, const ASTExpr *expr) {
    if (!expr) {
        return 0;
    }

    switch (expr->kind) {
    case EXPR_ADDR_OF: {
        const Symbol *symbol = expr->unary.target->symbol;

        return symbol ? symbol->scope_depth : 0;
    }
    case EXPR_VARIABLE:
        if (!expr->symbol) {
            return 0;
        }

        if (type_holds_its_memory_inline(expr->type)) {
            return expr->symbol->scope_depth;
        }

        return flow_get(pass->flow, expr->symbol).inner_depth;
    case EXPR_NEW:

        return 0;
    case EXPR_CALL: {
        if (!expr->type || type_kind(expr->type) != TYPE_REF) {
            return 0;
        }

        int deepest = 0;

        for (size_t i = 0; i < expr->call.args.size; i++) {
            int depth = inner_depth(pass, expr->call.args.data[i]);

            if (depth > deepest) {
                deepest = depth;
            }
        }

        return deepest;
    }
    case EXPR_FIELD:
        return inner_depth(pass, expr->field.target);

    case EXPR_LEND:
        return inner_depth(pass, expr->lend.target);

    case EXPR_DEREF:

        if (expr->unary.target->type && type_kind(expr->unary.target->type) == TYPE_BOX) {
            return 0;
        }

        return inner_depth(pass, expr->unary.target);
    default:
        return 0;
    }
}

static bool owning_field_of_local(const ASTExpr *expr, Symbol **out_symbol, unsigned int *out_index) {
    if (expr->kind != EXPR_FIELD || expr->field.target->kind != EXPR_VARIABLE) {
        return false;
    }

    Symbol *symbol = expr->field.target->symbol;
    const Type *struct_type = expr->field.target->type;

    if (!symbol || symbol->kind != SYMBOL_VAR || !struct_type || type_kind(struct_type) != TYPE_STRUCT) {
        return false;
    }

    const TypeField *field = ast_field_of(expr);

    if (!field || type_kind(field->type) != TYPE_BOX) {
        return false;
    }

    size_t index = 0;

    for (size_t i = 0; i < type_field_count(struct_type); i++) {
        const TypeField *other = &type_fields(struct_type)[i];

        if (other == field) {
            break;
        }

        if (type_kind(other->type) == TYPE_BOX) {
            index++;
        }
    }

    if (index >= FLOW_MAX_FIELDS) {
        return false;
    }

    *out_symbol = symbol;
    *out_index = (unsigned int)index;

    return true;
}

static uint64_t initialized_fields(FlowPass *pass, ASTExpr *initializer) {
    if (!initializer) {
        return 0;
    }

    ASTExpr *source = initializer;

    if (source->kind == EXPR_VARIABLE && source->symbol && source->symbol->kind == SYMBOL_VAR) {
        return flow_get(pass->flow, source->symbol).written_fields;
    }

    return UINT64_MAX;
}

static bool borrows_memory(const Type *type) {
    if (!type) {
        return false;
    }

    return type_is_indirect(type);
}

static void check_borrow_lifetime(FlowPass *pass, ASTExpr *value, int target_depth, Span span,
                                  const char *what) {
    if (!value) {
        return;
    }

    int depth = inner_depth(pass, value);

    if (depth == 0 || depth <= target_depth) {
        return;
    }

    flow_report(pass, span, "this borrow outlives what it names, so it cannot be %s", what);
}

static void check_stored_lifetime(FlowPass *pass, ASTExpr *value, const Type *destination, int target_depth,
                                  Span span, const char *what) {
    if (!borrows_memory(destination)) {
        return;
    }

    check_borrow_lifetime(pass, value, target_depth, span, what);
}

static void flow_pass_expr_list(FlowPass *pass, ASTExprList *list) {
    for (size_t i = 0; i < list->size; i++) {
        flow_pass_expr(pass, list->data[i]);
    }
}

static void flow_pass_expr(FlowPass *pass, ASTExpr *expr) {
    if (!expr) {
        return;
    }

    switch (expr->kind) {
    case EXPR_VARIABLE: {
        Symbol *entry = expr->symbol;

        if (!entry || entry->kind != SYMBOL_VAR || pass->assigning) {
            break;
        }

        FlowInit init = flow_get(pass->flow, entry).init;

        if (init == FLOW_MOVED) {
            char *name = string_ref_to_cstr(expr->var.name);
            flow_report(pass, expr->span, "'%s' was moved out of and no longer holds a value", name);
            free(name);
        } else if (init == FLOW_UNINIT && entry->var.type && type_is_indirect(entry->var.type)) {
            char *name = string_ref_to_cstr(expr->var.name);
            flow_report(pass, expr->span, "'%s' is read before it is given a value", name);
            free(name);
        }

        if (expr->moves) {
            FlowSlot slot = flow_get(pass->flow, entry);

            slot.init = FLOW_MOVED;
            flow_set(pass->flow, entry, slot);
        }

        break;
    }
    case EXPR_FIELD: {
        bool stored_into = pass->assigning_field;
        pass->assigning_field = false;

        bool assigning = pass->assigning;
        pass->assigning = false;

        flow_pass_expr(pass, expr->field.target);

        pass->assigning = assigning;

        Symbol *field_owner;
        unsigned int field_index;

        if (!stored_into && owning_field_of_local(expr, &field_owner, &field_index)) {
            FlowSlot slot = flow_get(pass->flow, field_owner);

            if (!(slot.written_fields & ((uint64_t)1 << field_index))) {
                flow_report(pass, expr->span, "'%s' is read before it is given a value",
                            ast_field_of(expr)->name->data);
            }
        }
        break;
    }
    case EXPR_LEND:
        flow_pass_expr(pass, expr->lend.target);
        break;

    case EXPR_ADDR_OF:
    case EXPR_DEREF:
    case EXPR_NEG:
    case EXPR_NOT:
        flow_pass_expr(pass, expr->unary.target);
        break;

    case EXPR_INDEX:
        flow_pass_expr(pass, expr->index.target);
        flow_pass_expr(pass, expr->index.index);
        break;
    case EXPR_BIN_OP:
        flow_pass_expr(pass, expr->bin_op.left);
        flow_pass_expr(pass, expr->bin_op.right);
        break;
    case EXPR_CALL:
        flow_pass_expr(pass, expr->call.target);
        flow_pass_expr_list(pass, &expr->call.args);
        break;
    case EXPR_CAST:
        flow_pass_expr(pass, expr->cast.operand);
        break;
    default:
        break;
    }
}

static void flow_pass_stmt(FlowPass *pass, ASTStmt *stmt) {
    switch (stmt->kind) {
    case STMT_EXPR:
        flow_pass_expr(pass, stmt->expr.value);
        break;
    case STMT_IF:
        flow_pass_expr(pass, stmt->ifstmt.condition);
        break;
    case STMT_FOR:
        flow_pass_expr(pass, stmt->forstmt.condition);
        break;
    case STMT_RETURN:
        flow_pass_expr(pass, stmt->ret.result);

        check_stored_lifetime(pass, stmt->ret.result, pass->return_type, 0, stmt->span, "returned");
        break;
    case STMT_VAR_DECL: {
        flow_pass_expr(pass, stmt->var_decl.initializer);

        Symbol *var = stmt->var_decl.symbol;

        if (!var) {
            break;
        }

        flow_set(pass->flow, var,
                 (FlowSlot){.init = stmt->var_decl.initializer ? FLOW_INIT : FLOW_UNINIT,
                            .inner_depth = inner_depth(pass, stmt->var_decl.initializer),
                            .written_fields = initialized_fields(pass, stmt->var_decl.initializer)});
        break;
    }
    case STMT_ASSIGN: {
        pass->assigning = stmt->assign.target->kind == EXPR_VARIABLE;
        pass->assigning_field = stmt->assign.target->kind == EXPR_FIELD;

        flow_pass_expr(pass, stmt->assign.target);

        pass->assigning = false;
        pass->assigning_field = false;

        flow_pass_expr(pass, stmt->assign.value);

        if (stmt->assign.target->kind == EXPR_FIELD || stmt->assign.target->kind == EXPR_DEREF) {
            check_stored_lifetime(pass, stmt->assign.value, stmt->assign.target->type, 0, stmt->span,
                                  "stored here");

            Symbol *field_owner;
            unsigned int field_index;

            if (owning_field_of_local(stmt->assign.target, &field_owner, &field_index)) {
                FlowSlot slot = flow_get(pass->flow, field_owner);

                slot.written_fields |= (uint64_t)1 << field_index;
                flow_set(pass->flow, field_owner, slot);
            }
            break;
        }

        Symbol *target = stmt->assign.target->symbol;

        if (target && target->kind == SYMBOL_VAR) {
            check_stored_lifetime(pass, stmt->assign.value, stmt->assign.target->type, target->scope_depth,
                                  stmt->span, "assigned here");

            FlowSlot slot = flow_get(pass->flow, target);

            flow_set(pass->flow, target,
                     (FlowSlot){.init = FLOW_INIT,
                                .inner_depth = inner_depth(pass, stmt->assign.value),
                                .written_fields = stmt->assign.value->type &&
                                                          type_kind(stmt->assign.value->type) == TYPE_STRUCT
                                                      ? initialized_fields(pass, stmt->assign.value)
                                                      : slot.written_fields});
        }
        break;
    }
    case STMT_COMPOUND_ASSIGN:
        flow_pass_expr(pass, stmt->compound_assign.target);
        flow_pass_expr(pass, stmt->compound_assign.value);
        break;
    default:
        break;
    }
}

static void flow_pass_block(FlowPass *pass, CFGBlock *block) {
    for (size_t i = 0; i < block->stmts.size; i++) {
        flow_pass_stmt(pass, block->stmts.data[i]);
    }
}

void flow_pass_run(Arena *arena, ASTStmt *body, Symbol **params, size_t param_count, const Type *return_type,
                   Diagnostics *diagnostics) {
    CFG *cfg = cfg_build(arena, body);

    Flow *entries = arena_alloc(arena, cfg->block_count * sizeof(Flow));

    for (size_t i = 0; i < cfg->block_count; i++) {
        flow_init(&entries[i], arena);
        entries[i].unreachable = true;
    }

    entries[cfg->entry->index].unreachable = false;

    for (size_t i = 0; i < param_count; i++) {
        if (params[i]) {
            flow_set(&entries[cfg->entry->index], params[i],
                     (FlowSlot){.init = FLOW_INIT, .inner_depth = 0, .written_fields = UINT64_MAX});
        }
    }

    FlowPass pass = {
        .arena = arena, .diagnostics = diagnostics, .reporting = false, .return_type = return_type};

    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t i = 0; i < cfg->block_count; i++) {
            CFGBlock *block = cfg->blocks[i];

            if (entries[i].unreachable) {
                continue;
            }

            Flow exit;
            flow_init(&exit, arena);
            flow_copy(&exit, &entries[i]);

            pass.flow = &exit;
            flow_pass_block(&pass, block);

            CFGBlock *successors[] = {block->sequential, block->branch};

            for (size_t s = 0; s < 2; s++) {
                if (!successors[s]) {
                    continue;
                }

                Flow *target = &entries[successors[s]->index];

                Flow merged;
                flow_init(&merged, arena);
                flow_copy(&merged, target);
                flow_merge(&merged, &exit);

                if (!flow_equals(&merged, target)) {
                    flow_copy(target, &merged);
                    changed = true;
                }
            }
        }
    }

    pass.reporting = true;

    for (size_t i = 0; i < cfg->block_count; i++) {
        if (entries[i].unreachable) {
            continue;
        }

        Flow exit;
        flow_init(&exit, arena);
        flow_copy(&exit, &entries[i]);

        pass.flow = &exit;
        flow_pass_block(&pass, cfg->blocks[i]);
    }
}
