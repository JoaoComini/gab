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

    TypeRegistry *registry;

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
        const Binding *binding = ast_root_local(expr->unary.target);

        return binding ? binding->scope_depth : 0;
    }
    case EXPR_VARIABLE:
        if (!ast_binding_of(expr)) {
            return 0;
        }

        if (type_registry_holds_its_memory_inline(pass->registry, expr->type)) {
            return ast_binding_of(expr)->scope_depth;
        }

        /* An owning slot frees its object when it goes out of scope, so a borrow of it lives no longer. */
        if (expr->type && type_kind(expr->type) == TYPE_BOX) {
            return ast_binding_of(expr)->scope_depth;
        }

        return flow_get(pass->flow, ast_binding_of(expr)).inner_depth;
    case EXPR_BOX:

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

/* The slot a borrow was taken from, so reassigning that slot can invalidate this one. */
/* Collects the slots a value borrows from, so freeing any of them can invalidate it. */
static void collect_borrow_sources(FlowPass *pass, const ASTExpr *value, FlowSlot *into) {
    if (!value) {
        return;
    }

    if (value->kind == EXPR_STRUCT_LIT) {
        for (size_t i = 0; i < value->struct_lit.fields.size; i++) {
            collect_borrow_sources(pass, value->struct_lit.fields.data[i].value, into);
        }

        return;
    }

    if (value->kind == EXPR_ARRAY_LIT) {
        for (size_t i = 0; i < value->array_lit.elements.size; i++) {
            collect_borrow_sources(pass, value->array_lit.elements.data[i], into);
        }

        return;
    }

    if (!value->type || !type_is_indirect(value->type)) {
        return;
    }

    Binding *root = ast_root_local(value);

    if (!root || root->kind != BINDING_VAR) {
        return;
    }

    if (root->var.type && type_kind(root->var.type) == TYPE_BOX) {
        flow_slot_add_source(into, root);
        return;
    }

    FlowSlot source = flow_get(pass->flow, root);

    for (size_t i = 0; i < source.borrow_count; i++) {
        flow_slot_add_source(into, source.borrows_from[i]);
    }
}

/* Only a borrowing destination is bound by what it names; an owning one takes the object with it. */
static bool borrows_memory(const Type *type) {
    if (!type) {
        return false;
    }

    return type_kind(type) == TYPE_REF;
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
        Binding *entry = ast_binding_of(expr);

        if (!entry || entry->kind != BINDING_VAR || pass->assigning) {
            break;
        }

        FlowInit init = flow_get(pass->flow, entry).init;

        if (init == FLOW_MOVED) {
            char *name = string_ref_to_cstr(expr->var.name);
            flow_report(pass, expr->span, "'%s' was moved out of and no longer holds a value", name);
            free(name);
        } else if (init == FLOW_DANGLING) {
            char *name = string_ref_to_cstr(expr->var.name);
            flow_report(pass, expr->span, "'%s' names memory that has been freed", name);
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
        pass->assigning_field = false;

        bool assigning = pass->assigning;
        pass->assigning = false;

        flow_pass_expr(pass, expr->field.target);

        pass->assigning = assigning;

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
    case EXPR_STRUCT_LIT:
        for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
            flow_pass_expr(pass, expr->struct_lit.fields.data[i].value);
        }
        break;
    case EXPR_ARRAY_LIT:
        flow_pass_expr_list(pass, &expr->array_lit.elements);
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

        Binding *var = stmt->var_decl.binding;

        if (!var) {
            break;
        }

        FlowSlot declared = {.init = stmt->var_decl.initializer ? FLOW_INIT : FLOW_UNINIT,
                             .inner_depth = inner_depth(pass, stmt->var_decl.initializer)};

        collect_borrow_sources(pass, stmt->var_decl.initializer, &declared);

        flow_set(pass->flow, var, declared);
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

            break;
        }

        Binding *target = ast_binding_of(stmt->assign.target);

        if (target && target->kind == BINDING_VAR) {
            check_stored_lifetime(pass, stmt->assign.value, stmt->assign.target->type, target->scope_depth,
                                  stmt->span, "assigned here");

            if (target->var.type && type_kind(target->var.type) == TYPE_BOX) {
                flow_invalidate_borrows_of(pass->flow, target);
            }

            FlowSlot assigned = {.init = FLOW_INIT, .inner_depth = inner_depth(pass, stmt->assign.value)};

            collect_borrow_sources(pass, stmt->assign.value, &assigned);

            flow_set(pass->flow, target, assigned);
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

void flow_pass_run(Arena *arena, TypeRegistry *registry, ASTStmt *body, Binding **params, size_t param_count,
                   const Type *return_type, Diagnostics *diagnostics) {
    CFG *cfg = cfg_build(arena, body);

    Flow *entries = arena_alloc(arena, cfg->block_count * sizeof(Flow));

    for (size_t i = 0; i < cfg->block_count; i++) {
        flow_init(&entries[i], arena);
        entries[i].unreachable = true;
    }

    entries[cfg->entry->index].unreachable = false;

    for (size_t i = 0; i < param_count; i++) {
        if (params[i]) {
            flow_set(&entries[cfg->entry->index], params[i], (FlowSlot){.init = FLOW_INIT, .inner_depth = 0});
        }
    }

    FlowPass pass = {.arena = arena,
                     .diagnostics = diagnostics,
                     .registry = registry,
                     .reporting = false,
                     .return_type = return_type};

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
