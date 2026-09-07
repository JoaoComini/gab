#include "mir/mir_drop.h"

#include <string.h>

static MIRInst *block_insert(Arena *arena, MIRBlock *block, size_t at, MIRInst inst) {
    if (block->inst_count == block->inst_capacity) {
        size_t capacity = block->inst_capacity == 0 ? 8 : block->inst_capacity * 2;

        MIRInst *grown = arena_alloc(arena, capacity * sizeof(MIRInst));

        memcpy(grown, block->insts, block->inst_count * sizeof(MIRInst));

        block->insts = grown;
        block->inst_capacity = capacity;
    }

    memmove(&block->insts[at + 1], &block->insts[at], (block->inst_count - at) * sizeof(MIRInst));

    block->insts[at] = inst;
    block->inst_count++;

    return &block->insts[at];
}

/* Whether a value is handed on rather than kept, which is what makes a producer's result someone
 * else's to end: stored into a place, or given to a call that takes it. */
/* Whether a callee takes the argument at this position, rather than only borrowing it; a borrowed
 * argument stays the caller's to end. */
static bool callee_takes(TypeRegistry *registry, const Function *callee, size_t index) {
    if (!callee || index >= callee->param_count) {
        return false;
    }

    return type_registry_owns(registry, callee->params[index]);
}

static bool is_given_away(TypeRegistry *registry, const MIRFunction *ir, MIRValueId value) {
    for (size_t b = 0; b < ir->block_count; b++) {
        const MIRBlock *block = ir->blocks[b];

        for (size_t j = 0; j < block->inst_count; j++) {
            const MIRInst *inst = &block->insts[j];

            if (inst->op != MIR_STORE && inst->op != MIR_CALL && inst->op != MIR_RETURN &&
                inst->op != MIR_BOX && inst->op != MIR_MAKE_SLICE) {
                continue;
            }

            bool is_call = inst->op == MIR_CALL;

            for (size_t a = 0; a < inst->arg_count; a++) {
                MIRValueId arg = mir_operand_as_value(inst->args[a]);

                if (mir_value_is_none(arg) || arg.id != value.id) {
                    continue;
                }

                if (is_call && !callee_takes(registry, inst->callee, a)) {
                    continue;
                }

                return true;
            }
        }
    }

    return false;
}

/* The instruction a value is last read at, which is where a temporary holding its own object ends. */
static bool last_read_of(const MIRFunction *ir, MIRValueId value, size_t *block_index, size_t *at) {
    bool found = false;

    for (size_t b = 0; b < ir->block_count; b++) {
        const MIRBlock *block = ir->blocks[b];

        for (size_t j = 0; j < block->inst_count; j++) {
            const MIRInst *inst = &block->insts[j];

            bool reads = false;

            for (size_t a = 0; a < inst->arg_count && !reads; a++) {
                MIRValueId arg = mir_operand_as_value(inst->args[a]);

                reads = !mir_value_is_none(arg) && arg.id == value.id;
            }

            if (!reads && mir_op_has_place(inst->op) && !mir_value_is_none(inst->place.base) &&
                inst->place.base.id == value.id) {
                reads = true;
            }

            if (reads) {
                *block_index = b;
                *at = j;
                found = true;
            }
        }
    }

    return found;
}

/* A temporary holding its own object is nobody's to end but the body's, so it is dropped where it is
 * last read; one stored into a place or handed to a call ends with whatever took it. */
static void drop_owned_temporaries(Arena *arena, TypeRegistry *registry, MIRFunction *ir) {
    for (size_t b = 0; b < ir->block_count; b++) {
        MIRBlock *block = ir->blocks[b];

        for (size_t j = 0; j < block->inst_count; j++) {
            const MIRInst *inst = &block->insts[j];

            if (inst->op != MIR_BOX || mir_value_is_none(inst->result)) {
                continue;
            }

            const MIRValueInfo *info = mir_value_info(ir, inst->result);

            if (!info || !mir_type_needs_drop(registry, info->type)) {
                continue;
            }

            MIRValueId result = inst->result;
            const Type *type = info->type;
            Span span = inst->span;

            if (is_given_away(registry, ir, result)) {
                continue;
            }

            size_t at_block = b;
            size_t at = j;

            /* A result nothing reads ends where it is made, since no later point names it. */
            last_read_of(ir, result, &at_block, &at);

            MIRBlock *target = ir->blocks[at_block];

            /* A terminator ends its block, so what it reads is dropped before it rather than after. */
            size_t insert_at = mir_op_is_terminator(target->insts[at].op) ? at : at + 1;

            block_insert(arena, target, insert_at,
                         (MIRInst){.op = MIR_DROP,
                                   .type = type,
                                   .result = MIR_NO_VALUE,
                                   .place = mir_place_of(result, NULL),
                                   .span = span});

            if (at_block == b && insert_at <= j) {
                j++;
            }
        }
    }
}

/* Storing over a place that already holds an owned value ends what it held, which is read out ahead
 * of the store so the release still names it; the first store into a place ends nothing. */
static void release_before_overwrite(Arena *arena, TypeRegistry *registry, MIRFunction *ir) {
    bool *stored = arena_alloc(arena, (ir->value_count + 1) * sizeof(bool));

    memset(stored, 0, (ir->value_count + 1) * sizeof(bool));

    for (size_t b = 0; b < ir->block_count; b++) {
        MIRBlock *block = ir->blocks[b];

        for (size_t j = 0; j < block->inst_count; j++) {
            const MIRInst *inst = &block->insts[j];

            if (inst->op == MIR_NULL && !mir_value_is_none(inst->place.base) &&
                inst->place.base.id < ir->value_count) {
                stored[inst->place.base.id] = false;
                continue;
            }

            if (inst->op != MIR_STORE || inst->place.projection_count != 0 ||
                mir_value_is_none(inst->place.base) || inst->place.base.id >= ir->value_count) {
                continue;
            }

            if (!mir_type_needs_drop(registry, inst->type) ||
                (inst->type && type_kind(inst->type) == TYPE_REF)) {
                continue;
            }

            MIRValueId base = inst->place.base;

            if (!stored[base.id]) {
                stored[base.id] = true;
                continue;
            }

            Place place = inst->place;
            const Type *type = inst->type;
            Span span = inst->span;

            MIRValueId held = mir_value_create(ir, type, NULL, span);

            block_insert(arena, block, j,
                         (MIRInst){.op = MIR_LOAD,
                                   .type = type,
                                   .result = held,
                                   .place = place,
                                   .read = READ_COPY,
                                   .span = span});

            block_insert(arena, block, j + 2,
                         (MIRInst){.op = MIR_DROP,
                                   .type = type,
                                   .result = MIR_NO_VALUE,
                                   .place = mir_place_of(held, NULL),
                                   .span = span});

            j += 2;
        }
    }
}

static void block_remove(MIRBlock *block, size_t at) {
    memmove(&block->insts[at], &block->insts[at + 1], (block->inst_count - at - 1) * sizeof(MIRInst));

    block->inst_count--;
}

/* A place emptied by a move and not filled again holds nothing, so the drop it still reaches frees
 * nothing and is dropped itself. */
static void drop_releases_of_emptied_places(MIRFunction *ir) {
    for (size_t b = 0; b < ir->block_count; b++) {
        MIRBlock *block = ir->blocks[b];

        bool *emptied = NULL;

        for (size_t j = 0; j < block->inst_count;) {
            const MIRInst *inst = &block->insts[j];

            if (!emptied) {
                emptied = arena_alloc(ir->arena, (ir->value_count + 1) * sizeof(bool));
                memset(emptied, 0, (ir->value_count + 1) * sizeof(bool));
            }

            if (!mir_op_has_place(inst->op) || mir_value_is_none(inst->place.base) ||
                inst->place.base.id >= ir->value_count) {
                j++;
                continue;
            }

            size_t base = inst->place.base.id;

            if (inst->op == MIR_NULL && inst->place.projection_count == 0) {
                emptied[base] = true;
            } else if (inst->op == MIR_STORE || inst->op == MIR_STORAGE_INIT) {
                emptied[base] = false;
            } else if (inst->op == MIR_DROP && inst->place.projection_count == 0 && emptied[base]) {
                block_remove(block, j);
                continue;
            }

            j++;
        }
    }
}

void mir_drop_elaborate(Arena *arena, TypeRegistry *registry, MIRFunction *ir) {
    for (size_t b = 0; b < ir->block_count; b++) {
        MIRBlock *block = ir->blocks[b];

        for (size_t i = 0; i < block->inst_count;) {
            MIRInst *inst = &block->insts[i];

            /* Giving a value away leaves its slot holding nothing, so a later release frees nothing. */
            if (inst->op == MIR_LOAD && inst->read == READ_MOVE) {
                if (!mir_type_needs_drop(registry, inst->type)) {
                    i++;
                    continue;
                }

                Place place = inst->place;
                Span span = inst->span;
                const Type *type = inst->type;

                block_insert(
                    arena, block, i + 1,
                    (MIRInst){
                        .op = MIR_NULL, .type = type, .result = MIR_NO_VALUE, .place = place, .span = span});

                i += 2;
                continue;
            }

            i++;
        }
    }

    release_before_overwrite(arena, registry, ir);

    drop_owned_temporaries(arena, registry, ir);

    drop_releases_of_emptied_places(ir);
}
