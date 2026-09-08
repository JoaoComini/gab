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

/* What a slot holds where control flow reaches it, which decides whether its drop frees anything.
 * A slot holds its object until something empties it, so that is what an unwalked block assumes. */
typedef enum {
    HOLDS_ITS_OBJECT = 0,
    HOLDS_NOTHING,

    /* Reached by paths that disagree, so only a flag written along them can say. */
    HOLDS_EITHER,
} Holding;

static Holding holding_merge(Holding a, Holding b) { return a == b ? a : HOLDS_EITHER; }

/* Walks one block from what it was entered holding, answering what it leaves holding. A visitor is
 * given each drop with what the slot holds there, which the rewriting walk uses and the fixpoint does not. */
static void walk_block(MIRFunction *ir, MIRBlock *block, Holding *holds,
                       void (*visit)(void *context, MIRBlock *block, size_t at, Holding held),
                       void *context) {
    for (size_t j = 0; j < block->inst_count; j++) {
        const MIRInst *inst = &block->insts[j];

        if (!mir_op_has_place(inst->op) || mir_value_is_none(inst->place.base) ||
            inst->place.base.id >= ir->value_count) {
            continue;
        }

        size_t base = inst->place.base.id;

        if (inst->place.projection_count != 0) {
            continue;
        }

        if (inst->op == MIR_NULL) {
            holds[base] = HOLDS_NOTHING;
        } else if (inst->op == MIR_STORE || inst->op == MIR_STORAGE_INIT) {
            holds[base] = HOLDS_ITS_OBJECT;
        } else if (inst->op == MIR_DROP && visit) {
            visit(context, block, j, holds[base]);
        }
    }
}

/* What every block is entered holding, once every path into it agrees. */
static Holding *entry_holdings(Arena *arena, MIRFunction *ir) {
    size_t width = ir->value_count + 1;

    Holding *entries = arena_alloc(arena, ir->block_count * width * sizeof(Holding));

    memset(entries, 0, ir->block_count * width * sizeof(Holding));

    Holding *exit = arena_alloc(arena, width * sizeof(Holding));

    /* An entry is what its predecessors leave, so one is built afresh each round rather than merged
     * into the last round's answer, which no merge could ever lower again. */
    Holding *next = arena_alloc(arena, ir->block_count * width * sizeof(Holding));
    bool *reached = arena_alloc(arena, ir->block_count * sizeof(bool));

    bool changed = true;

    while (changed) {
        changed = false;

        memset(next, 0, ir->block_count * width * sizeof(Holding));
        memset(reached, 0, ir->block_count * sizeof(bool));

        reached[ir->entry.id] = true;

        for (size_t b = 0; b < ir->block_count; b++) {
            memcpy(exit, &entries[b * width], width * sizeof(Holding));

            walk_block(ir, ir->blocks[b], exit, NULL, NULL);

            MIRBlockId successors[2];
            size_t count = mir_block_successors(ir->blocks[b], successors);

            for (size_t s = 0; s < count; s++) {
                size_t target = successors[s].id;

                if (!reached[target]) {
                    memcpy(&next[target * width], exit, width * sizeof(Holding));
                    reached[target] = true;
                    continue;
                }

                for (size_t v = 0; v < width; v++) {
                    next[target * width + v] = holding_merge(next[target * width + v], exit[v]);
                }
            }
        }

        if (memcmp(entries, next, ir->block_count * width * sizeof(Holding)) != 0) {
            memcpy(entries, next, ir->block_count * width * sizeof(Holding));
            changed = true;
        }
    }

    return entries;
}

typedef struct {
    MIRFunction *ir;

    /* Collected rather than removed in place, so the walk is not rewriting what it reads. */
    MIRBlock **blocks;
    size_t *indices;
    size_t count;
} EmptyDrops;

static void note_empty_drop(void *context, MIRBlock *block, size_t at, Holding held) {
    EmptyDrops *found = context;

    if (held != HOLDS_NOTHING) {
        return;
    }

    found->blocks[found->count] = block;
    found->indices[found->count] = at;
    found->count++;
}

typedef struct {
    MIRFunction *ir;

    /* The flag each slot is guarded by, none for a slot whose paths agree. */
    MIRValueId *flags;
} Flagged;

/* Marks every drop whose slot's paths disagree, so the walk that follows can write the flag it reads. */
static void note_conditional_drop(void *context, MIRBlock *block, size_t at, Holding held) {
    Flagged *flagged = context;

    if (held != HOLDS_EITHER) {
        return;
    }

    MIRInst *inst = &block->insts[at];

    size_t base = inst->place.base.id;

    if (mir_value_is_none(flagged->flags[base])) {
        const Type *bool_type = type_registry_get_primitive(flagged->ir->registry, TYPE_BOOL);

        flagged->flags[base] = mir_value_create(flagged->ir, bool_type, NULL, inst->span);
    }

    inst->flag = flagged->flags[base];
}

/* Writes the flag beside every point that changes what a slot holds, so the drop reading it is answered
 * on every path rather than only the one that moved. */
static void write_drop_flags(Arena *arena, MIRFunction *ir, const MIRValueId *flags) {
    for (size_t b = 0; b < ir->block_count; b++) {
        MIRBlock *block = ir->blocks[b];

        for (size_t j = 0; j < block->inst_count;) {
            const MIRInst *inst = &block->insts[j];

            if (!mir_op_has_place(inst->op) || mir_value_is_none(inst->place.base) ||
                inst->place.base.id >= ir->value_count || inst->place.projection_count != 0) {
                j++;
                continue;
            }

            MIRValueId flag = flags[inst->place.base.id];

            if (mir_value_is_none(flag)) {
                j++;
                continue;
            }

            bool holds = inst->op == MIR_STORE || inst->op == MIR_STORAGE_INIT;

            /* A flag starts saying the slot holds nothing, so opening its scope writes nothing itself. */
            if (!holds && inst->op != MIR_NULL) {
                j++;
                continue;
            }

            Place place = inst->place;
            Span span = inst->span;

            const Type *bool_type = type_registry_get_primitive(ir->registry, TYPE_BOOL);

            MIROperand *args = mir_args_alloc(ir, 1);
            args[0] = mir_operand_const(constant_bool(bool_type, holds));

            block_insert(arena, block, j + 1,
                         (MIRInst){.op = MIR_DROP_FLAG,
                                   .type = bool_type,
                                   .result = MIR_NO_VALUE,
                                   .args = args,
                                   .arg_count = 1,
                                   .place = place,
                                   .flag = flag,
                                   .span = span});

            j += 2;
        }
    }
}

/* A drop whose paths disagree is guarded by a flag those paths write, rather than by what the slot
 * itself was left holding, so nothing depends on a moved-from slot reading as zero. */
static void guard_conditional_drops(Arena *arena, MIRFunction *ir) {
    if (ir->block_count == 0) {
        return;
    }

    size_t width = ir->value_count + 1;

    Holding *entries = entry_holdings(arena, ir);

    Flagged flagged = {.ir = ir, .flags = arena_alloc(arena, width * sizeof(MIRValueId))};

    for (size_t i = 0; i < width; i++) {
        flagged.flags[i] = MIR_NO_VALUE;
    }

    Holding *holds = arena_alloc(arena, width * sizeof(Holding));

    for (size_t b = 0; b < ir->block_count; b++) {
        memcpy(holds, &entries[b * width], width * sizeof(Holding));

        walk_block(ir, ir->blocks[b], holds, note_conditional_drop, &flagged);
    }

    write_drop_flags(arena, ir, flagged.flags);

    /* The flag now says what a moved-from slot used to say by reading as zero, so the nulling goes. */
    for (size_t b = 0; b < ir->block_count; b++) {
        MIRBlock *block = ir->blocks[b];

        for (size_t j = 0; j < block->inst_count;) {
            const MIRInst *inst = &block->insts[j];

            if (inst->op == MIR_NULL && !mir_value_is_none(inst->place.base) &&
                inst->place.projection_count == 0 && inst->place.base.id < ir->value_count &&
                !mir_value_is_none(flagged.flags[inst->place.base.id])) {
                block_remove(block, j);
                continue;
            }

            j++;
        }
    }
}

/* A place every path empties holds nothing where its drop stands, so that drop frees nothing and goes. */
static void drop_releases_of_emptied_places(Arena *arena, MIRFunction *ir) {
    if (ir->block_count == 0) {
        return;
    }

    size_t width = ir->value_count + 1;

    Holding *entries = entry_holdings(arena, ir);

    size_t drops = 0;

    for (size_t b = 0; b < ir->block_count; b++) {
        drops += ir->blocks[b]->inst_count;
    }

    EmptyDrops found = {.ir = ir,
                        .blocks = arena_alloc(arena, drops * sizeof(MIRBlock *)),
                        .indices = arena_alloc(arena, drops * sizeof(size_t))};

    Holding *holds = arena_alloc(arena, width * sizeof(Holding));

    for (size_t b = 0; b < ir->block_count; b++) {
        memcpy(holds, &entries[b * width], width * sizeof(Holding));

        walk_block(ir, ir->blocks[b], holds, note_empty_drop, &found);
    }

    /* Removed last to first within a block, so an earlier index is still the instruction it named. */
    for (size_t i = found.count; i > 0; i--) {
        block_remove(found.blocks[i - 1], found.indices[i - 1]);
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

    drop_releases_of_emptied_places(arena, ir);

    guard_conditional_drops(arena, ir);
}
