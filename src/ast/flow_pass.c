#include "ast/flow_pass.h"

#include "ast/cfg.h"
#include "ast/expr.h"
#include "ast/flow.h"
#include "type.h"

#include <string.h>

// What the transfer function carries: the state being updated, and where to
// report. No scope and no symbol table -- every name this pass reads was bound
// by resolution and is on the node.
typedef struct {
    Flow *flow;
    Arena *arena;
    Diagnostics *diagnostics;

    // What a 'return' is checked against. Held here because a returned borrow
    // is not always visible in the returned expression's type: a 'str'
    // is a header copy, so no address-of node marks it.
    TypeHandle return_type;

    // Set on the last round only. The lattice is iterated to a fixpoint, so an
    // intermediate round can see a state a later one refutes; reporting from
    // those would produce errors that the converged answer does not support.
    bool reporting;

    // Set while the outermost field of an assignment target is visited. 'h.b'
    // in 'h.b = v' is stored into rather than read, so it is what makes the
    // field readable; a nested 'h.b' in 'h.b.n = v' clears this and is read.
    bool assigning_field;

    // Set while the target of an assignment is visited. A plain 'x = v' writes
    // x rather than reading it, so a dead x is revived by the write instead of
    // being an error.
    bool assigning;
} FlowPass;

static void flow_pass_expr(FlowPass *pass, ASTExpr *expr);

static void flow_report(FlowPass *pass, Span span, const char *format, const char *arg) {
    if (!pass->reporting) {
        return;
    }

    diag_error(pass->diagnostics, GAB_ERR_LIFETIME, span, format, arg);
}

// A variable's depth comes from the flow state, so what this answers is what
// holds on every path reaching the expression rather than whatever the most
// recent assignment happened to store.
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

        // An owning string holds its characters in its own slot, so they live
        // exactly as long as the variable does. A pointer variable is the other
        // case: what it names was decided wherever it was assigned, which is
        // what the lattice carries.
        if (expr->type && expr->type->kind == TYPE_STRING && type_is_owned(expr->type)) {
            return expr->symbol->scope_depth;
        }

        return flow_get(pass->flow, expr->symbol).inner_depth;
    case EXPR_NEW:
        // A heap object outlives every frame, so 0 is the truth here rather
        // than the "unknown" the default stands for.
        return 0;
    case EXPR_CALL: {
        // A call handing back a 'ref T' borrows from an argument; which one is
        // not knowable without a per-function summary, so the result is taken
        // to borrow from the shortest-lived of them. Conservative in one
        // direction only -- it can refuse a borrow of something longer-lived,
        // never accept one that dangles.
        if (!expr->type || expr->type->kind != TYPE_REF) {
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
    case EXPR_DEREF:
        // Reading through a borrow reaches no further than the borrow does: what
        // '*s' names lives as long as whatever 's' was made from, so the hops a
        // lend inserts carry that depth rather than resetting it to 0.
        //
        // An owning pointer is the exception -- what it names is a heap object,
        // which outlives every frame however the pointer was reached.
        if (expr->unary.target->type && expr->unary.target->type->kind == TYPE_BOX) {
            return 0;
        }

        return inner_depth(pass, expr->unary.target);
    default:
        return 0;
    }
}

// Whether this expression is a struct local's owning pointer field -- 'h.b'
// where h is a struct variable and b owns. Those are the fields codegen nulls
// at the declaration, so those are the ones whose written-ness is tracked.
static bool owning_field_of_local(const ASTExpr *expr, Symbol **out_symbol, unsigned int *out_index) {
    if (expr->kind != EXPR_FIELD || expr->field.target->kind != EXPR_VARIABLE) {
        return false;
    }

    Symbol *symbol = expr->field.target->symbol;
    TypeHandle struct_type = expr->field.target->type;

    if (!symbol || symbol->kind != SYMBOL_VAR || !struct_type || struct_type->kind != TYPE_STRUCT) {
        return false;
    }

    const TypeField *field = expr->field.field;

    if (!field || field->type->kind != TYPE_BOX) {
        return false;
    }

    // The bit is this field's position among the struct's *owning* fields, not
    // among all of them. A struct is mostly scalars in practice -- it mirrors a
    // host struct -- and numbering by raw position would spend the set on
    // fields that can never be unwritten, so a lone owning pointer behind sixty
    // ints would fall off the end.
    size_t index = 0;

    for (size_t i = 0; i < type_field_count(struct_type); i++) {
        const TypeField *other = &type_fields(struct_type)[i];

        if (other == field) {
            break;
        }

        if (other->type->kind == TYPE_BOX) {
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

// The written-field set a declaration's initializer hands its new variable.
// Taking a whole struct makes the destination's fields exactly as written as
// the source's were, so the set travels with the value.
static uint64_t initialized_fields(FlowPass *pass, ASTExpr *initializer) {
    if (!initializer) {
        return 0;
    }

    ASTExpr *source = initializer->kind == EXPR_MOVE ? initializer->unary.target : initializer;

    if (source->kind == EXPR_VARIABLE && source->symbol && source->symbol->kind == SYMBOL_VAR) {
        return flow_get(pass->flow, source->symbol).written_fields;
    }

    return UINT64_MAX;
}

// Whether a value names memory it does not own, so that how long that memory
// lives is this pass's business. True of a 'ref T' and of a 'str': the
// two are different representations -- one an address, one a header carrying
// one -- and the lifetime question is the same for both.
static bool borrows_memory(TypeHandle type) {
    if (!type) {
        return false;
    }

    return type_is_indirect(type) || (type->kind == TYPE_STRING && !type_is_owned(type));
}

// Rejects a borrow being stored somewhere that outlives what it names.
// 'target_depth' is the block depth of the destination; a inner declared
// deeper than that is gone by the time the destination can still be read.
static void check_borrow_lifetime(FlowPass *pass, ASTExpr *value, int target_depth, Span span,
                                  const char *what) {
    if (!value) {
        return;
    }

    int depth = inner_depth(pass, value);

    // 0 means the inner is unknown, which is not evidence of a problem.
    if (depth == 0 || depth <= target_depth) {
        return;
    }

    flow_report(pass, span, "this borrow outlives what it names, so it cannot be %s", what);
}

// As check_borrow_lifetime, for a destination whose type says whether a borrow
// is formed. Reading the destination rather than the value is what catches a
// string: a 'str' takes an owning 'String' by copying its header, so the
// value still reads as owning and only the destination shows a borrow was made.
static void check_stored_lifetime(FlowPass *pass, ASTExpr *value, TypeHandle destination, int target_depth,
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
            // Only a pointer. An unwritten slot holds whatever the frame last
            // left there: read as an int that is a wrong answer, read as a
            // pointer it is an address nothing chose.
            char *name = string_ref_to_cstr(expr->var.name);
            flow_report(pass, expr->span, "'%s' is read before it is given a value", name);
            free(name);
        }
        break;
    }
    case EXPR_FIELD: {
        bool stored_into = pass->assigning_field;
        pass->assigning_field = false;

        // The target is read even where the field is written: 'h.b = v' still
        // names h. Only the field itself is exempt.
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
                            expr->field.field->name->data);
            }
        }
        break;
    }
    case EXPR_MOVE: {
        flow_pass_expr(pass, expr->unary.target);

        Symbol *source = expr->unary.target->symbol;

        // Moving out of anything but a named slot has nothing to kill: a
        // temporary already owns what it produced.
        if (!source || source->kind != SYMBOL_VAR || expr->unary.target->kind == EXPR_FIELD) {
            break;
        }

        // Only 'init' changes: the slot is dead, but what its fields hold is
        // what the destination now receives.
        FlowSlot slot = flow_get(pass->flow, source);

        slot.init = FLOW_MOVED;
        flow_set(pass->flow, source, slot);
        break;
    }
    case EXPR_ADDR_OF:
    case EXPR_DEREF:
    case EXPR_NEG:
    case EXPR_NOT:
        flow_pass_expr(pass, expr->unary.target);
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

// One statement's effect on the state. Only the statements a block holds reach
// here: an 'if' or a 'for' contributes its condition, since the branching
// itself is the graph's business rather than the transfer function's.
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

        // A returned pointer outlives the whole frame, so nothing declared
        // inside the function may be pointed at. Depth 0 is the global scope.
        //
        // Checked against the declared return type rather than the returned
        // expression's: returning an owning string where a 'str' was
        // declared forms a borrow that outlives the slot holding the
        // characters, and the expression alone still reads as owning.
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

        // Storing through a pointer reaches something whose lifetime this
        // frame does not bound, so only a pointer to something equally
        // long-lived may be stored there.
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

            // The variable now points at whatever was just stored in it. What
            // its fields hold comes from the value, exactly as at a
            // declaration.
            flow_set(pass->flow, target,
                     (FlowSlot){.init = FLOW_INIT,
                                .inner_depth = inner_depth(pass, stmt->assign.value),
                                .written_fields =
                                    stmt->assign.value->type && stmt->assign.value->type->kind == TYPE_STRUCT
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

// Runs one block's statements over a copy of the state arriving at it.
static void flow_pass_block(FlowPass *pass, CFGBlock *block) {
    for (size_t i = 0; i < block->stmts.size; i++) {
        flow_pass_stmt(pass, block->stmts.data[i]);
    }
}

void flow_pass_run(Arena *arena, ASTStmt *body, Symbol **params, size_t param_count, TypeHandle return_type,
                   Diagnostics *diagnostics) {
    CFG *cfg = cfg_build(arena, body);

    // Per-block entry state: what holds on every edge into that block. A block
    // no edge has reached yet stays unreachable, which is the merge's identity.
    Flow *entries = arena_alloc(arena, cfg->block_count * sizeof(Flow));

    for (size_t i = 0; i < cfg->block_count; i++) {
        flow_init(&entries[i], arena);
        entries[i].unreachable = true;
    }

    entries[cfg->entry->index].unreachable = false;

    // A parameter was supplied by the caller, so it holds a value and its
    // fields are whatever the caller's were -- nothing here may call one
    // unwritten.
    for (size_t i = 0; i < param_count; i++) {
        if (params[i]) {
            flow_set(&entries[cfg->entry->index], params[i],
                     (FlowSlot){.init = FLOW_INIT, .inner_depth = 0, .written_fields = UINT64_MAX});
        }
    }

    FlowPass pass = {
        .arena = arena, .diagnostics = diagnostics, .reporting = false, .return_type = return_type};

    // Iterate until nothing changes. Each round recomputes every block's exit
    // from its entry and merges that into its successors' entries; a merge
    // that adds nothing leaves the lattice where it was, so the round that
    // changes no entry is the fixpoint.
    //
    // It terminates because every merge is monotone: 'init' only ever weakens,
    // 'inner_depth' only ever deepens, and 'written_fields' only ever loses
    // bits, over a finite set of slots.
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

    // The lattice has converged, so one more round over the same states is
    // what the program actually means. Only this round reports.
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
