#include "mir/mir_borrowck.h"

#include "mir/mir_liveness.h"
#include "mir/mir_state.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    MIRState *state;
    Arena *arena;
    Diagnostics *diagnostics;

    TypeRegistry *registry;
    MIRFunction *ir;
    const Liveness *liveness;

    const Type *return_type;

    bool reporting;

    uint32_t returned_params;

    MIRBlockId block;
    size_t index;
} MIRFlow;

static void report(MIRFlow *flow, Span span, const char *format, const char *arg) {
    if (!flow->reporting) {
        return;
    }

    diag_error(flow->diagnostics, GAB_ERR_LIFETIME, span, format, arg);
}

static bool call_result_may_name(const Function *callee, size_t index) {
    if (!callee || !callee->borrowed_params_known) {
        return true;
    }

    return index >= 32 || (callee->borrowed_params & ((uint32_t)1 << index)) != 0;
}

static Symbol *binding_of(const MIRFlow *flow, MIRValueId value) {
    const MIRValueInfo *info = mir_value_info(flow->ir, value);

    return info ? info->binding : NULL;
}

static bool borrows_memory(const MIRFlow *flow, const Type *type) {
    return type && type_registry_borrows(flow->registry, type);
}

static MIRValueId root_of(const Place *place) { return place->base; }

static bool is_parameter(const MIRFlow *flow, MIRValueId value) {
    for (size_t i = 0; i < flow->ir->param_count; i++) {
        if (flow->ir->params[i].id == value.id) {
            return true;
        }
    }

    return false;
}

static const Type *type_of(const MIRFlow *flow, MIRValueId value) {
    const MIRValueInfo *info = mir_value_info(flow->ir, value);

    return info ? info->type : NULL;
}

static bool holds_its_own_object(MIRFlow *flow, MIRValueId value) {
    const Type *type = type_of(flow, value);

    return type && type_kind(type) == TYPE_BOX;
}

static bool slot_of(MIRFlow *flow, const Place *place, MIRSlot *out) {
    MIRValueId root = root_of(place);

    if (mir_value_is_none(root)) {
        return false;
    }

    MIRSlot slot = mir_state_get(flow->state, root);

    for (size_t i = 0; i < place->projection_count; i++) {
        if (place->projections[i].kind != PROJ_FIELD) {
            break;
        }

        if (place->projections[i].field.id >= slot.field_count) {
            return false;
        }

        slot = slot.fields[place->projections[i].field.id];
    }

    *out = slot;

    return true;
}

static bool store_into(MIRFlow *flow, const Place *place, size_t depth, MIRSlot stored) {
    MIRValueId root = root_of(place);

    if (mir_value_is_none(root)) {
        return false;
    }

    if (depth == 0) {
        mir_state_set(flow->state, root, stored);
        return true;
    }

    MIRSlot slot = mir_state_get(flow->state, root);
    MIRSlot *at = &slot;

    for (size_t i = 0; i < depth; i++) {
        if (place->projections[i].kind != PROJ_FIELD || place->projections[i].field.id >= at->field_count) {
            return false;
        }

        at = &at->fields[place->projections[i].field.id];
    }

    *at = stored;

    mir_state_set(flow->state, root, slot);

    return true;
}

static size_t tracked_depth(const Place *place) {
    size_t depth = 0;

    while (depth < place->projection_count && place->projections[depth].kind == PROJ_FIELD) {
        depth++;
    }

    return depth;
}

static bool reaches_past_its_root(const Place *place) {
    for (size_t i = 0; i < place->projection_count; i++) {
        if (place->projections[i].kind == PROJ_DEREF) {
            return true;
        }
    }

    return false;
}

static const MIRInst *definition_of(const MIRFlow *flow, MIRValueId value) {
    if (mir_value_is_none(value)) {
        return NULL;
    }

    for (size_t i = 0; i < flow->ir->block_count; i++) {
        const MIRBlock *block = flow->ir->blocks[i];

        for (size_t j = 0; j < block->inst_count; j++) {
            if (!mir_value_is_none(block->insts[j].result) && block->insts[j].result.id == value.id) {
                return &block->insts[j];
            }
        }
    }

    return NULL;
}

static void collect_sources(MIRFlow *flow, MIRValueId value, MIRSlot *into);

static void collect_place_sources(MIRFlow *flow, const Place *place, MIRSlot *into) {
    MIRValueId root = root_of(place);

    if (mir_value_is_none(root)) {
        return;
    }

    if (holds_its_own_object(flow, root)) {
        mir_slot_add_borrow(flow->arena, into, root);
        return;
    }

    MIRSlot named;

    if (!slot_of(flow, place, &named)) {
        named = mir_state_get(flow->state, root);
    }

    MIRSlot flat = mir_slot_flattened(flow->arena, &named);

    for (size_t i = 0; i < flat.borrow_count; i++) {
        mir_slot_add_borrow(flow->arena, into, flat.borrows[i]);
    }

    if (flat.borrow_count == 0) {
        mir_slot_add_borrow(flow->arena, into, root);
    }
}

static void collect_sources(MIRFlow *flow, MIRValueId value, MIRSlot *into) {
    const MIRInst *inst = definition_of(flow, value);

    if (!inst) {
        const Type *type = type_of(flow, value);

        MIRSlot named = mir_state_get(flow->state, value);
        MIRSlot flat = mir_slot_flattened(flow->arena, &named);

        for (size_t i = 0; i < flat.borrow_count; i++) {
            mir_slot_add_borrow(flow->arena, into, flat.borrows[i]);
        }

        if (flat.borrow_count > 0) {
            return;
        }

        if (borrows_memory(flow, type) || holds_its_own_object(flow, value)) {
            mir_slot_add_borrow(flow->arena, into, value);
        }

        return;
    }

    switch (inst->op) {
    case MIR_LOAD:
    case MIR_REF:
        collect_place_sources(flow, &inst->place, into);
        return;

    case MIR_COPY:
    case MIR_MAKE_SLICE:
        for (size_t i = 0; i < inst->arg_count; i++) {
            collect_sources(flow, mir_operand_as_value(inst->args[i]), into);
        }
        return;

    case MIR_BOX:
        return;

    case MIR_CALL:
        for (size_t i = 0; i < inst->arg_count; i++) {
            if (call_result_may_name(inst->callee, i)) {
                collect_sources(flow, mir_operand_as_value(inst->args[i]), into);
            }
        }
        return;

    default:
        break;
    }

    if (!borrows_memory(flow, inst->type)) {
        return;
    }

    for (size_t i = 0; i < inst->arg_count; i++) {
        collect_sources(flow, mir_operand_as_value(inst->args[i]), into);
    }
}

static bool live_where_storage_ends(MIRFlow *flow, MIRValueId source, MIRValueId held_in, MIRFieldId field) {
    for (size_t i = 0; i < flow->ir->block_count; i++) {
        const MIRBlock *block = flow->ir->blocks[i];

        for (size_t j = 0; j < block->inst_count; j++) {
            const MIRInst *inst = &block->insts[j];

            if (inst->op != MIR_STORAGE_DEAD && inst->op != MIR_DROP) {
                continue;
            }

            if (inst->place.base.id != source.id) {
                continue;
            }

            if (mir_live_after(flow->liveness, block->id, j, held_in, field)) {
                return true;
            }
        }
    }

    return false;
}

static bool outlives_its_source(MIRFlow *flow, MIRValueId borrow, MIRValueId held_in, bool held_in_field,
                                MIRFieldId field) {
    MIRSlot sources = {0};

    collect_sources(flow, borrow, &sources);

    for (size_t i = 0; i < sources.borrow_count; i++) {
        MIRValueId source = sources.borrows[i];

        if (is_parameter(flow, source)) {
            continue;
        }

        const Type *type = type_of(flow, source);

        if (type && type_kind(type) == TYPE_BOX) {
            continue;
        }

        if (mir_value_is_none(held_in)) {
            return true;
        }

        if (live_where_storage_ends(flow, source, held_in, held_in_field ? field : MIR_WHOLE_VALUE)) {
            return true;
        }
    }

    return false;
}

static MIRSlotInit state_of(MIRFlow *flow, const Place *place) {
    MIRSlot named;

    if (!slot_of(flow, place, &named)) {
        named = mir_state_get(flow->state, root_of(place));
    }

    return named.init == MIR_SLOT_INIT ? mir_slot_flattened(flow->arena, &named).init : named.init;
}

static void check_readable(MIRFlow *flow, const Place *place, Span span) {
    MIRValueId root = root_of(place);

    if (mir_value_is_none(root)) {
        return;
    }

    switch (state_of(flow, place)) {
    case MIR_SLOT_MOVED:
        report(flow, span, "this was moved out of and no longer holds a value", NULL);
        break;
    case MIR_SLOT_DANGLING:
        report(flow, span, "this names memory that has been freed", NULL);
        break;
    case MIR_SLOT_UNINIT:
        if (type_is_indirect(type_of(flow, root))) {
            report(flow, span, "this is read before it is given a value", NULL);
        }
        break;
    default:
        break;
    }
}

static void flow_inst(MIRFlow *flow, const MIRInst *inst) {
    switch (inst->op) {
    case MIR_STORAGE_LIVE:
        mir_state_set(flow->state, root_of(&inst->place), (MIRSlot){.init = MIR_SLOT_UNINIT});
        break;

    case MIR_STORAGE_INIT: {
        MIRValueId root = root_of(&inst->place);
        MIRSlot slot = mir_state_get(flow->state, root);

        slot.init = MIR_SLOT_INIT;

        mir_state_set(flow->state, root, slot);
        break;
    }

    case MIR_STORAGE_DEAD: {
        MIRValueId root = root_of(&inst->place);

        mir_state_invalidate_borrows_of(flow->state, root);

        mir_state_set(flow->state, root, (MIRSlot){.init = MIR_SLOT_UNINIT});
        break;
    }

    case MIR_DROP:
        mir_state_invalidate_borrows_of(flow->state, root_of(&inst->place));

        for (size_t i = 0; i < inst->arg_count; i++) {
            mir_state_invalidate_borrows_of(flow->state, mir_operand_as_value(inst->args[i]));
        }
        break;

    case MIR_LOAD: {
        check_readable(flow, &inst->place, inst->span);

        MIRSlot loaded = {.init = MIR_SLOT_INIT};

        collect_place_sources(flow, &inst->place, &loaded);

        mir_state_set(flow->state, inst->result, loaded);

        if (inst->read != READ_MOVE) {
            break;
        }

        MIRValueId root = root_of(&inst->place);
        MIRSlot slot = mir_state_get(flow->state, root);

        mir_slot_set_all(&slot, MIR_SLOT_MOVED);

        mir_state_set(flow->state, root, slot);
        break;
    }

    case MIR_REF: {
        check_readable(flow, &inst->place, inst->span);

        MIRSlot taken = {.init = MIR_SLOT_INIT};

        collect_place_sources(flow, &inst->place, &taken);

        mir_state_set(flow->state, inst->result, taken);
        break;
    }

    case MIR_STORE: {
        if (inst->arg_count == 0) {
            break;
        }

        MIRValueId root = root_of(&inst->place);

        if (mir_value_is_none(root)) {
            break;
        }

        size_t depth = tracked_depth(&inst->place);

        if (reaches_past_its_root(&inst->place)) {
            if (borrows_memory(flow, type_of(flow, mir_operand_as_value(inst->args[0]))) &&
                outlives_its_source(flow, mir_operand_as_value(inst->args[0]), MIR_NO_VALUE, false,
                                    MIR_WHOLE_VALUE)) {
                report(flow, inst->span, "this borrow outlives what it names, so it cannot be stored here",
                       NULL);
            }

            break;
        }

        MIRSlot stored = {.init = MIR_SLOT_INIT};

        collect_sources(flow, mir_operand_as_value(inst->args[0]), &stored);

        const Type *type = type_of(flow, root);

        if (depth == 0 && type && type_kind(type) == TYPE_BOX) {
            mir_state_invalidate_borrows_of(flow->state, root);
        }

        if (depth > 0) {
            MIRSlot owner = mir_state_get(flow->state, root);

            owner.init = MIR_SLOT_INIT;

            MIRSlot *at = &owner;

            for (size_t i = 0; i < depth; i++) {
                mir_slot_open_fields(at, flow->arena, inst->place.projections[i].field.id + 1);

                at = &at->fields[inst->place.projections[i].field.id];
            }

            mir_state_set(flow->state, root, owner);
        }

        if (!store_into(flow, &inst->place, depth, stored)) {
            MIRSlot slot = mir_state_get(flow->state, root);

            slot.init = MIR_SLOT_INIT;

            collect_sources(flow, mir_operand_as_value(inst->args[0]), &slot);

            mir_state_set(flow->state, root, slot);
        }

        if (borrows_memory(flow, type_of(flow, mir_operand_as_value(inst->args[0]))) &&
            outlives_its_source(flow, mir_operand_as_value(inst->args[0]), root, depth > 0,
                                depth > 0 ? inst->place.projections[0].field : MIR_WHOLE_VALUE)) {
            report(flow, inst->span, "this borrow outlives what it names, so it cannot be stored here", NULL);
        }
        break;
    }

    case MIR_CALL:
    case MIR_BOX:
    case MIR_MAKE_SLICE:
    case MIR_COPY: {
        for (size_t i = 0; i < inst->arg_count; i++) {
            MIRSlot named = mir_state_get(flow->state, mir_operand_as_value(inst->args[i]));

            if (named.init == MIR_SLOT_UNREACHED) {
                continue;
            }

            MIRSlot flat = mir_slot_flattened(flow->arena, &named);

            if (flat.init == MIR_SLOT_DANGLING) {
                report(flow, inst->span, "this names memory that has been freed", NULL);
            } else if (flat.init == MIR_SLOT_MOVED) {
                report(flow, inst->span, "this was moved out of and no longer holds a value", NULL);
            }
        }

        MIRSlot produced = {.init = MIR_SLOT_INIT};

        collect_sources(flow, inst->result, &produced);

        mir_state_set(flow->state, inst->result, produced);
        break;
    }

    case MIR_RETURN: {
        if (inst->arg_count == 0 || !borrows_memory(flow, flow->return_type)) {
            break;
        }

        MIRSlot reached = {0};
        collect_sources(flow, mir_operand_as_value(inst->args[0]), &reached);

        for (size_t i = 0; i < reached.borrow_count; i++) {
            for (size_t p = 0; p < flow->ir->param_count; p++) {
                if (flow->ir->params[p].id == reached.borrows[i].id) {
                    flow->returned_params |= (uint32_t)1 << p;
                }
            }
        }

        MIRSlot returned = mir_state_get(flow->state, mir_operand_as_value(inst->args[0]));

        if (returned.init != MIR_SLOT_UNREACHED &&
            mir_slot_flattened(flow->arena, &returned).init == MIR_SLOT_DANGLING) {
            report(flow, inst->span, "this names memory that has been freed", NULL);
            break;
        }

        if (outlives_its_source(flow, mir_operand_as_value(inst->args[0]), MIR_NO_VALUE, false,
                                MIR_WHOLE_VALUE)) {
            report(flow, inst->span, "this borrow outlives what it names, so it cannot be returned", NULL);
        }
        break;
    }

    default:
        break;
    }
}

static void flow_block(MIRFlow *flow, const MIRBlock *block) {
    flow->block = block->id;

    for (size_t i = 0; i < block->inst_count; i++) {
        flow->index = i;

        flow_inst(flow, &block->insts[i]);
    }
}

#define MIR_FLOW_SCRATCH_ARENA_BLOCK_SIZE 2048

void mir_borrowck(Arena *arena, TypeRegistry *registry, MIRFunction *ir, Diagnostics *diagnostics,
                  Function *function, bool report_diagnostics) {
    if (ir->block_count == 0) {
        return;
    }

    Liveness *liveness = mir_liveness_compute(arena, ir);

    Arena *scratch = arena_create(MIR_FLOW_SCRATCH_ARENA_BLOCK_SIZE);

    MIRState *entries = arena_alloc(arena, ir->block_count * sizeof(MIRState));

    for (size_t i = 0; i < ir->block_count; i++) {
        mir_state_init(&entries[i], arena);
        entries[i].unreachable = true;
    }

    entries[ir->entry.id].unreachable = false;

    MIRFlow flow = {.arena = arena,
                    .diagnostics = diagnostics,
                    .registry = registry,
                    .ir = ir,
                    .liveness = liveness,
                    .return_type = function ? function->signature.return_type : NULL,
                    .reporting = false};

    for (size_t i = 0; i < ir->param_count; i++) {
        const MIRValueInfo *info = mir_value_info(ir, ir->params[i]);

        MIRSlot slot = {.init = MIR_SLOT_INIT};

        if (info && borrows_memory(&flow, info->type)) {
            mir_slot_add_borrow(arena, &slot, ir->params[i]);
        }

        mir_state_set(&entries[ir->entry.id], ir->params[i], slot);
    }

    flow.arena = scratch;

    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t i = 0; i < ir->block_count; i++) {
            if (entries[i].unreachable) {
                continue;
            }

            ArenaCheckpoint checkpoint = arena_checkpoint(scratch);

            MIRState exit;
            mir_state_init(&exit, scratch);
            mir_state_copy(&exit, &entries[i]);

            flow.state = &exit;
            flow_block(&flow, ir->blocks[i]);

            MIRBlockId successors[2];
            size_t count = mir_block_successors(ir->blocks[i], successors);

            for (size_t s = 0; s < count; s++) {
                MIRState *target = &entries[successors[s].id];

                MIRState merged;
                mir_state_init(&merged, scratch);
                mir_state_copy(&merged, target);
                mir_state_merge(&merged, &exit);

                if (!mir_state_equals(&merged, target)) {
                    mir_state_copy(target, &merged);
                    changed = true;
                }
            }

            arena_rewind(scratch, checkpoint);
        }
    }

    flow.reporting = report_diagnostics;

    for (size_t i = 0; i < ir->block_count; i++) {
        if (entries[i].unreachable) {
            continue;
        }

        ArenaCheckpoint checkpoint = arena_checkpoint(scratch);

        MIRState exit;
        mir_state_init(&exit, scratch);
        mir_state_copy(&exit, &entries[i]);

        flow.state = &exit;
        flow_block(&flow, ir->blocks[i]);

        arena_rewind(scratch, checkpoint);
    }

    arena_destroy(scratch);

    if (function && !report_diagnostics) {
        function->borrowed_params = flow.returned_params;
        function->borrowed_params_known = true;
    }
}
