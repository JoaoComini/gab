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

    Binding **params;
    size_t param_count;

    uint32_t returned_params;
} FlowPass;

static void flow_pass_expr(FlowPass *pass, ASTExpr *expr);

static void flow_report(FlowPass *pass, Span span, const char *format, const char *arg) {
    if (!pass->reporting) {
        return;
    }

    diag_error(pass->diagnostics, GAB_ERR_LIFETIME, span, format, arg);
}

/* A call's result may name this argument, either because the callee says so or because nothing does. */
static bool call_result_may_name(const Function *callee, size_t index) {
    if (!callee || !callee->borrowed_params_known) {
        return true;
    }

    return index >= 32 || (callee->borrowed_params & ((uint32_t)1 << index)) != 0;
}

static int deepest_of(int a, int b) { return a > b ? a : b; }

/* The tracked slot an lvalue names, following each field into its own state. */
static bool slot_of_place(FlowPass *pass, const ASTExpr *place, FlowSlot *out) {
    if (!place) {
        return false;
    }

    if (place->kind == EXPR_VARIABLE) {
        Binding *binding = ast_binding_of(place);

        if (!binding || binding->kind != BINDING_VAR) {
            return false;
        }

        *out = flow_get(pass->flow, binding);

        return true;
    }

    if (place->kind != EXPR_FIELD) {
        return false;
    }

    FlowSlot owner;

    if (!slot_of_place(pass, place->field.target, &owner) || place->field.index >= owner.field_count) {
        return false;
    }

    *out = owner.fields[place->field.index];

    return true;
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

        {
            FlowSlot named = flow_get(pass->flow, ast_binding_of(expr));

            return flow_slot_flattened(pass->arena, &named).inner_depth;
        }
    case EXPR_BOX:

        return 0;
    case EXPR_CALL: {
        if (!type_registry_borrows(pass->registry, expr->type)) {
            return 0;
        }

        int deepest = 0;

        for (size_t i = 0; i < expr->call.args.size; i++) {
            if (!call_result_may_name(expr->callee, i)) {
                continue;
            }

            deepest = deepest_of(deepest, inner_depth(pass, expr->call.args.data[i]));
        }

        return deepest;
    }
    case EXPR_STRUCT_LIT: {
        int deepest = 0;

        for (size_t i = 0; i < expr->struct_lit.fields.size; i++) {
            deepest = deepest_of(deepest, inner_depth(pass, expr->struct_lit.fields.data[i].value));
        }

        return deepest;
    }
    case EXPR_ARRAY_LIT: {
        int deepest = 0;

        for (size_t i = 0; i < expr->array_lit.elements.size; i++) {
            deepest = deepest_of(deepest, inner_depth(pass, expr->array_lit.elements.data[i]));
        }

        return deepest;
    }
    case EXPR_FIELD: {
        FlowSlot named;

        if (slot_of_place(pass, expr, &named)) {
            return flow_slot_flattened(pass->arena, &named).inner_depth;
        }

        return inner_depth(pass, expr->field.target);
    }

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

    if (value->kind == EXPR_CALL) {
        for (size_t i = 0; i < value->call.args.size; i++) {
            if (call_result_may_name(value->callee, i)) {
                collect_borrow_sources(pass, value->call.args.data[i], into);
            }
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
        flow_slot_add_source(pass->arena, into, root);
        return;
    }

    FlowSlot named;

    if (!slot_of_place(pass, value, &named)) {
        named = flow_get(pass->flow, root);
    }

    FlowSlot source = flow_slot_flattened(pass->arena, &named);

    for (size_t i = 0; i < source.borrow_count; i++) {
        flow_slot_add_source(pass->arena, into, source.borrows_from[i]);
    }
}

/* The state a value leaves in the slot it is stored into, per field where the value names them. */
static FlowSlot slot_of_value(FlowPass *pass, const ASTExpr *value) {
    FlowSlot slot = {.init = value ? FLOW_INIT : FLOW_UNINIT, .inner_depth = inner_depth(pass, value)};

    if (!value || value->kind != EXPR_STRUCT_LIT) {
        collect_borrow_sources(pass, value, &slot);

        return slot;
    }

    /* Sources live on the fields that named them, so the struct itself names none of them. */
    flow_slot_open_fields(&slot, pass->arena, value->struct_lit.fields.size);

    for (size_t i = 0; i < value->struct_lit.fields.size; i++) {
        const ASTFieldInit *init = &value->struct_lit.fields.data[i];

        if (init->index < slot.field_count) {
            slot.fields[init->index] = slot_of_value(pass, init->value);
        }
    }

    return slot;
}

/* Only a borrowing destination is bound by what it names; an owning one takes the object with it. */
static bool borrows_memory(FlowPass *pass, const Type *type) {
    if (!type) {
        return false;
    }

    return type_registry_borrows(pass->registry, type);
}

/* Records which parameters a returned borrow reaches, so a caller can attribute the result to them. */
static void collect_returned_params(FlowPass *pass, const ASTExpr *value) {
    if (!value || !borrows_memory(pass, pass->return_type)) {
        return;
    }

    FlowSlot reached = {0};
    collect_borrow_sources(pass, value, &reached);

    for (size_t i = 0; i < reached.borrow_count; i++) {
        for (size_t p = 0; p < pass->param_count; p++) {
            if (pass->params[p] == reached.borrows_from[i]) {
                pass->returned_params |= (uint32_t)1 << p;
            }
        }
    }
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

/* How long the slot written through lives; a heap object outlives every scope that names it. */
static int destination_depth(FlowPass *pass, const ASTExpr *target) {
    while (target) {
        switch (target->kind) {
        case EXPR_VARIABLE: {
            Binding *binding = ast_binding_of(target);

            if (!binding || binding->kind != BINDING_VAR) {
                return 0;
            }

            return type_registry_owns(pass->registry, binding->var.type) ? 0 : binding->scope_depth;
        }
        case EXPR_FIELD:
            if (type_registry_owns(pass->registry, target->field.target->type)) {
                return 0;
            }

            target = target->field.target;
            break;
        case EXPR_INDEX:
            target = target->index.target;
            break;
        case EXPR_DEREF:
            return 0;
        default:
            return 0;
        }
    }

    return 0;
}

static void check_stored_lifetime(FlowPass *pass, ASTExpr *value, const Type *destination, int target_depth,
                                  Span span, const char *what) {
    if (!borrows_memory(pass, destination)) {
        return;
    }

    check_borrow_lifetime(pass, value, target_depth, span, what);
}

/* Writes 'stored' into the field an lvalue names; false when that field is not tracked apart. */
static bool store_into_place(FlowPass *pass, const ASTExpr *place, FlowSlot stored) {
    if (!place || place->kind != EXPR_FIELD) {
        return false;
    }

    const ASTExpr *owner = place->field.target;

    if (owner->kind == EXPR_VARIABLE) {
        Binding *binding = ast_binding_of(owner);

        if (!binding || binding->kind != BINDING_VAR) {
            return false;
        }

        FlowSlot slot = flow_get(pass->flow, binding);

        if (place->field.index >= slot.field_count) {
            return false;
        }

        slot.fields[place->field.index] = stored;

        flow_set(pass->flow, binding, slot);

        return true;
    }

    FlowSlot nested;

    if (!slot_of_place(pass, owner, &nested) || place->field.index >= nested.field_count) {
        return false;
    }

    nested.fields[place->field.index] = stored;

    return store_into_place(pass, owner, nested);
}

/* A field given a borrow names what that borrow names, leaving the struct's other fields alone. */
static void narrow_to_stored_borrow(FlowPass *pass, const ASTExpr *target, const ASTExpr *value) {
    if (target->kind != EXPR_FIELD) {
        return;
    }

    Binding *root = ast_root_local(target);

    if (!root || root->kind != BINDING_VAR || type_registry_owns(pass->registry, root->var.type)) {
        return;
    }

    FlowSlot stored = slot_of_value(pass, value);

    if (!store_into_place(pass, target, stored)) {
        FlowSlot slot = flow_get(pass->flow, root);

        slot.inner_depth = deepest_of(slot.inner_depth, stored.inner_depth);

        collect_borrow_sources(pass, value, &slot);

        flow_set(pass->flow, root, slot);
    }
}

static void flow_pass_expr_list(FlowPass *pass, ASTExprList *list) {
    for (size_t i = 0; i < list->size; i++) {
        flow_pass_expr(pass, list->data[i]);
    }
}

/* A field answers for itself only while the struct holding it still holds a value at all. */
static bool field_is_tracked(FlowPass *pass, const ASTExpr *field, FlowSlot *out) {
    Binding *root = ast_root_local(field);

    if (!root || root->kind != BINDING_VAR || flow_get(pass->flow, root).init != FLOW_INIT) {
        return false;
    }

    return slot_of_place(pass, field, out);
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

        FlowSlot named = flow_get(pass->flow, entry);

        /* Reading the whole value reads every field, so a field's freed borrow dangles the read. */
        FlowInit init = named.init == FLOW_INIT ? flow_slot_flattened(pass->arena, &named).init : named.init;

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

        FlowSlot named;

        /* A tracked field answers for itself; its struct may hold a freed borrow in another field. */
        if (!assigning && field_is_tracked(pass, expr, &named)) {
            if (flow_slot_flattened(pass->arena, &named).init == FLOW_DANGLING) {
                flow_report(pass, expr->span, "this field names memory that has been freed", NULL);
            }
        } else {
            flow_pass_expr(pass, expr->field.target);
        }

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

        collect_returned_params(pass, stmt->ret.result);

        check_stored_lifetime(pass, stmt->ret.result, pass->return_type, 0, stmt->span, "returned");
        break;
    case STMT_VAR_DECL: {
        flow_pass_expr(pass, stmt->var_decl.initializer);

        Binding *var = stmt->var_decl.binding;

        if (!var) {
            break;
        }

        flow_set(pass->flow, var, slot_of_value(pass, stmt->var_decl.initializer));
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
            int depth = destination_depth(pass, stmt->assign.target);

            check_stored_lifetime(pass, stmt->assign.value, stmt->assign.target->type, depth, stmt->span,
                                  "stored here");

            narrow_to_stored_borrow(pass, stmt->assign.target, stmt->assign.value);

            break;
        }

        Binding *target = ast_binding_of(stmt->assign.target);

        if (target && target->kind == BINDING_VAR) {
            check_stored_lifetime(pass, stmt->assign.value, stmt->assign.target->type, target->scope_depth,
                                  stmt->span, "assigned here");

            if (target->var.type && type_kind(target->var.type) == TYPE_BOX) {
                flow_invalidate_borrows_of(pass->flow, target);
            }

            flow_set(pass->flow, target, slot_of_value(pass, stmt->assign.value));
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

#define FLOW_SCRATCH_ARENA_BLOCK_SIZE 2048

void flow_pass_run(Arena *arena, TypeRegistry *registry, ASTStmt *body, Binding **params, size_t param_count,
                   const Type *return_type, Diagnostics *diagnostics, Function *function, bool report) {
    CFG *cfg = cfg_build(arena, body);

    /* Every flow the fixpoint loop builds besides entries[] is scratch: read only to decide what
     * entries[] should hold, and copied into it by value, so none of it needs to outlive an iteration.
     * It lives on its own arena so rewinding it can never reclaim entries[]'s own storage. */
    Arena *scratch = arena_create(FLOW_SCRATCH_ARENA_BLOCK_SIZE);

    Flow *entries = arena_alloc(arena, cfg->block_count * sizeof(Flow));

    for (size_t i = 0; i < cfg->block_count; i++) {
        flow_init(&entries[i], arena);
        entries[i].unreachable = true;
    }

    entries[cfg->entry->index].unreachable = false;

    FlowPass pass = {.arena = arena,
                     .diagnostics = diagnostics,
                     .registry = registry,
                     .reporting = false,
                     .return_type = return_type,
                     .params = params,
                     .param_count = param_count};

    for (size_t i = 0; i < param_count; i++) {
        if (params[i]) {
            FlowSlot slot = {.init = FLOW_INIT, .inner_depth = 0};

            if (borrows_memory(&pass, params[i]->var.type)) {
                flow_slot_add_source(arena, &slot, params[i]);
            }

            flow_set(&entries[cfg->entry->index], params[i], slot);
        }
    }

    /* Everything from here on mutates a flow that lives on scratch, so its field and source arrays
     * should too, or rewinding scratch would reclaim nothing and this arena would fill up instead. */
    pass.arena = scratch;

    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t i = 0; i < cfg->block_count; i++) {
            CFGBlock *block = cfg->blocks[i];

            if (entries[i].unreachable) {
                continue;
            }

            ArenaCheckpoint checkpoint = arena_checkpoint(scratch);

            Flow exit;
            flow_init(&exit, scratch);
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
                flow_init(&merged, scratch);
                flow_copy(&merged, target);
                flow_merge(&merged, &exit);

                if (!flow_equals(&merged, target)) {
                    flow_copy(target, &merged);
                    changed = true;
                }
            }

            arena_rewind(scratch, checkpoint);
        }
    }

    pass.reporting = report;

    for (size_t i = 0; i < cfg->block_count; i++) {
        if (entries[i].unreachable) {
            continue;
        }

        ArenaCheckpoint checkpoint = arena_checkpoint(scratch);

        Flow exit;
        flow_init(&exit, scratch);
        flow_copy(&exit, &entries[i]);

        pass.flow = &exit;
        flow_pass_block(&pass, cfg->blocks[i]);

        arena_rewind(scratch, checkpoint);
    }

    arena_destroy(scratch);

    if (function && !report) {
        function->borrowed_params = pass.returned_params;
        function->borrowed_params_known = true;
    }
}
