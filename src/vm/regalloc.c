#include "vm/regalloc.h"

#include "mir/mir_liveness.h"
#include "vm/slot.h"
#include "vm/opcode.h"

#include <string.h>

static unsigned int slots_for(TypeRegistry *registry, const Type *type) {
    if (!type) {
        return 1;
    }

    return (unsigned int)((type_registry_size_of(registry, type) + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);
}

static unsigned int align_for(TypeRegistry *registry, const Type *type) {
    if (!type || type_registry_align_of(registry, type) <= VM_SLOT_SIZE) {
        return 1;
    }

    return (unsigned int)(type_registry_align_of(registry, type) / VM_SLOT_SIZE);
}

/* Which values are ever live at one point, as one bit per pair, so a slot check is a lookup rather
 * than a walk of the whole body. */
typedef struct {
    uint64_t *bits;
    size_t value_count;
    size_t words_per_row;

    const MIRFunction *ir;
    const Liveness *liveness;

    /* The values live at the point being visited, gathered before they are paired off. */
    MIRValueId *at_point;
    size_t at_point_count;

    /* The same set as bits, so adding it to a row is an or rather than a mark for every pair. */
    uint64_t *at_point_bits;

    /* Which values hold storage at the point being visited; a local holds its slot from where its
     * storage opens until it ends, across every block in between, however late a store fills it. */
    bool *holds_storage;

    /* The block being walked, the point in it, and what holds storage entering each block. */
    const MIRBlock *block;
    size_t point;
    bool *storage_on_entry;

    Arena *arena;
} Interference;

static bool interferes(const Interference *graph, MIRValueId a, MIRValueId b) {
    if (a.id >= graph->value_count || b.id >= graph->value_count) {
        return false;
    }

    size_t index = a.id * graph->words_per_row * 64 + b.id;

    return (graph->bits[index / 64] & ((uint64_t)1 << (index % 64))) != 0;
}

/* The row of the matrix holding what this value interferes with, one bit per value. */
static const uint64_t *interference_row(const Interference *graph, MIRValueId value) {
    return graph->bits + value.id * graph->words_per_row;
}

/* Whether each value's storage is open on entry to each block, which a local's slot is held across
 * even where no read makes it live: it is claimed at 'storage_live' and released at 'storage_dead'. */
static void compute_storage_on_entry(Interference *graph, bool *on_entry) {
    const MIRFunction *ir = graph->ir;
    size_t count = graph->value_count;

    memset(on_entry, 0, ir->block_count * count * sizeof(bool));

    bool *open = arena_alloc(graph->arena, (count + 1) * sizeof(bool));

    for (bool changed = true; changed;) {
        changed = false;

        for (size_t b = 0; b < ir->block_count; b++) {
            const MIRBlock *block = ir->blocks[b];

            memcpy(open, on_entry + block->id.id * count, count * sizeof(bool));

            /* A value filled part by part holds its slots from the block's start, since a later part
             * must find what the earlier ones claimed still held. */
            for (size_t j = 0; j < block->inst_count; j++) {
                const MIRInst *inst = &block->insts[j];

                if (inst->op == MIR_STORE && inst->place.projection_count > 0 &&
                    !mir_value_is_none(inst->place.base) && inst->place.base.id < count) {
                    open[inst->place.base.id] = true;
                }
            }

            for (size_t j = 0; j < block->inst_count; j++) {
                const MIRInst *inst = &block->insts[j];

                if (!mir_op_has_place(inst->op) || mir_value_is_none(inst->place.base) ||
                    inst->place.base.id >= count) {
                    continue;
                }

                if (inst->op == MIR_STORAGE_LIVE ||
                    (inst->op == MIR_STORE && inst->place.projection_count > 0)) {
                    open[inst->place.base.id] = true;
                } else if (inst->op == MIR_STORAGE_DEAD) {
                    open[inst->place.base.id] = false;
                }
            }

            MIRBlockId successors[2];
            size_t successor_count = mir_block_successors(block, successors);

            for (size_t i = 0; i < successor_count; i++) {
                bool *into = on_entry + successors[i].id * count;

                for (size_t v = 0; v < count; v++) {
                    if (open[v] && !into[v]) {
                        into[v] = true;
                        changed = true;
                    }
                }
            }
        }
    }
}

/* Replays a block forwards to say what holds storage at each point, which the backward walk reads. */
static void set_storage_at(Interference *graph, const MIRBlock *block, const bool *on_entry, size_t point) {
    memcpy(graph->holds_storage, on_entry + block->id.id * graph->value_count,
           graph->value_count * sizeof(bool));

    /* A value filled part by part holds its slots throughout the block that fills it. */
    for (size_t j = 0; j < block->inst_count; j++) {
        const MIRInst *inst = &block->insts[j];

        if (inst->op == MIR_STORE && inst->place.projection_count > 0 &&
            !mir_value_is_none(inst->place.base) && inst->place.base.id < graph->value_count) {
            graph->holds_storage[inst->place.base.id] = true;
        }
    }

    for (size_t j = 0; j <= point && j < block->inst_count; j++) {
        const MIRInst *inst = &block->insts[j];

        if (!mir_op_has_place(inst->op) || mir_value_is_none(inst->place.base) ||
            inst->place.base.id >= graph->value_count) {
            continue;
        }

        if (inst->op == MIR_STORAGE_LIVE || (inst->op == MIR_STORE && inst->place.projection_count > 0)) {
            graph->holds_storage[inst->place.base.id] = true;
        } else if (inst->op == MIR_STORAGE_DEAD) {
            graph->holds_storage[inst->place.base.id] = false;
        }
    }
}

/* Every value live at one point conflicts with every other live there. */
static void visit_point(void *context, const uint64_t *live) {
    Interference *graph = context;

    /* The walk reports from the block's exit backwards, so each visit is one point earlier. */
    graph->point--;

    set_storage_at(graph, graph->block, graph->storage_on_entry, graph->point);

    graph->at_point_count = 0;

    memset(graph->at_point_bits, 0, graph->words_per_row * sizeof(uint64_t));

    for (size_t v = 0; v < graph->value_count; v++) {
        MIRValueId value = {(uint32_t)v};

        if (mir_live_set_holds(graph->liveness, live, value) || graph->holds_storage[v]) {
            graph->at_point[graph->at_point_count++] = value;
            graph->at_point_bits[v / 64] |= (uint64_t)1 << (v % 64);
        }
    }

    /* Everything here conflicts with everything else here, which is this set added to each of their
     * rows rather than a mark for every pair; a value's own bit is cleared after, as it conflicts
     * with itself no more than a slot conflicts with itself. */
    for (size_t i = 0; i < graph->at_point_count; i++) {
        uint64_t *row = graph->bits + graph->at_point[i].id * graph->words_per_row;

        for (size_t w = 0; w < graph->words_per_row; w++) {
            row[w] |= graph->at_point_bits[w];
        }

        row[graph->at_point[i].id / 64] &= ~((uint64_t)1 << (graph->at_point[i].id % 64));
    }

    /* A result is written to its slot whether or not it is ever read, so it conflicts with whatever
     * is live where it is defined; a dead result sharing one would overwrite what that slot holds. */
    MIRValueId result = graph->block->insts[graph->point].result;

    if (!mir_value_is_none(result) && result.id < graph->value_count) {
        uint64_t *row = graph->bits + result.id * graph->words_per_row;

        for (size_t w = 0; w < graph->words_per_row; w++) {
            row[w] |= graph->at_point_bits[w];
        }

        row[result.id / 64] &= ~((uint64_t)1 << (result.id % 64));

        /* The matrix is symmetric, so each value here records the result as a conflict of its own. */
        for (size_t i = 0; i < graph->at_point_count; i++) {
            if (graph->at_point[i].id == result.id) {
                continue;
            }

            uint64_t *other = graph->bits + graph->at_point[i].id * graph->words_per_row;

            other[result.id / 64] |= (uint64_t)1 << (result.id % 64);
        }
    }
}

static Interference *build_interference(Arena *arena, const MIRFunction *ir, const Liveness *liveness) {
    Interference *graph = arena_alloc(arena, sizeof(Interference));

    size_t words_per_row = (ir->value_count + 63) / 64;

    if (words_per_row == 0) {
        words_per_row = 1;
    }

    *graph = (Interference){
        .value_count = ir->value_count, .words_per_row = words_per_row, .ir = ir, .liveness = liveness};

    graph->bits = arena_alloc(arena, ir->value_count * words_per_row * sizeof(uint64_t));
    memset(graph->bits, 0, ir->value_count * words_per_row * sizeof(uint64_t));

    graph->at_point = arena_alloc(arena, (ir->value_count + 1) * sizeof(MIRValueId));
    graph->at_point_bits = arena_alloc(arena, words_per_row * sizeof(uint64_t));
    graph->holds_storage = arena_alloc(arena, (ir->value_count + 1) * sizeof(bool));
    graph->arena = arena;

    graph->storage_on_entry = arena_alloc(arena, ir->block_count * ir->value_count * sizeof(bool) + 1);

    compute_storage_on_entry(graph, graph->storage_on_entry);

    for (size_t i = 0; i < ir->block_count; i++) {
        graph->block = ir->blocks[i];
        graph->point = ir->blocks[i]->inst_count;

        mir_live_walk_block(liveness, ir->blocks[i]->id, visit_point, graph);
    }

    return graph;
}

RegAlloc *regalloc_run(Arena *arena, MIRFunction *ir, Diagnostics *diagnostics) {
    RegAlloc *alloc = arena_alloc(arena, sizeof(RegAlloc));

    *alloc = (RegAlloc){.value_count = ir->value_count};

    alloc->slots = arena_alloc(arena, (ir->value_count + 1) * sizeof(unsigned int));

    for (size_t i = 0; i < ir->value_count; i++) {
        alloc->slots[i] = REGALLOC_NO_SLOT;
    }

    /* Slot 0 is the return slot, and a parameter follows in the order the callee reads them. */
    unsigned int next = 1;

    for (size_t i = 0; i < ir->param_count; i++) {
        const MIRValueInfo *info = mir_value_info(ir, ir->params[i]);

        alloc->slots[ir->params[i].id] = next;

        next += slots_for(ir->registry, info ? info->type : NULL);
    }

    if (next > VM_MAX_FRAME_SLOTS) {
        diag_error(diagnostics, GAB_ERR_CODEGEN, (Span){0}, "function signature is too large for a frame");

        alloc->failed = true;

        return alloc;
    }

    Liveness *liveness = mir_liveness_compute(arena, ir);

    Interference *graph = build_interference(arena, ir, liveness);

    /* How wide each value is and what it aligns to, asked of the registry once rather than again for
     * every slot a search steps over. */
    unsigned int *widths = arena_alloc(arena, (ir->value_count + 1) * sizeof(unsigned int));
    unsigned int *aligns = arena_alloc(arena, (ir->value_count + 1) * sizeof(unsigned int));

    for (size_t i = 0; i < ir->value_count; i++) {
        widths[i] = slots_for(ir->registry, ir->values[i].type);
        aligns[i] = align_for(ir->registry, ir->values[i].type);
    }

    /* Which slots a value cannot take, one bit each, gathered from the values it interferes with that
     * already hold one. A frame is addressed by a byte, so every slot fits in a handful of words. */
    uint64_t blocked[(VM_MAX_FRAME_SLOTS + 64) / 64];

    for (size_t v = 0; v < ir->value_count; v++) {
        if (alloc->slots[v] != REGALLOC_NO_SLOT) {
            continue;
        }

        const MIRValueInfo *info = &ir->values[v];

        unsigned int width = widths[v];
        unsigned int align = aligns[v];

        unsigned int at = REGALLOC_NO_SLOT;

        memset(blocked, 0, sizeof(blocked));

        /* Walking the row a word at a time reaches only the values this one conflicts with, rather
         * than asking about every value that already holds a slot. */
        const uint64_t *row = interference_row(graph, (MIRValueId){(uint32_t)v});

        for (size_t w = 0; w < graph->words_per_row; w++) {
            for (uint64_t bits = row[w]; bits != 0; bits &= bits - 1) {
                size_t other = w * 64 + (size_t)__builtin_ctzll(bits);

                unsigned int held = alloc->slots[other];

                if (held == REGALLOC_NO_SLOT) {
                    continue;
                }

                unsigned int held_width = widths[other];

                /* A slot this value could start at overlaps the neighbour if the neighbour ends after
                 * it starts, so every start from 'held - width + 1' through 'held + held_width' is out. */
                unsigned int from = held > width ? held - width + 1 : 1;

                for (unsigned int slot = from; slot < held + held_width; slot++) {
                    blocked[slot / 64] |= (uint64_t)1 << (slot % 64);
                }
            }
        }

        /* A run of slots holding nothing live alongside this value can hold it instead of a new one. */
        /* Slot 0 holds the return value throughout, so reuse starts after it. */
        for (unsigned int candidate = 1; candidate + width <= next; candidate++) {
            if (align > 1 && candidate % align != 0) {
                continue;
            }

            if ((blocked[candidate / 64] & ((uint64_t)1 << (candidate % 64))) == 0) {
                at = candidate;
                break;
            }
        }

        if (at == REGALLOC_NO_SLOT) {
            if (align > 1 && next % align != 0) {
                next += align - next % align;
            }

            if (width > VM_MAX_FRAME_SLOTS || next > VM_MAX_FRAME_SLOTS - width) {
                diag_error(diagnostics, GAB_ERR_CODEGEN, info->span,
                           width > 1 ? "struct is too large for a frame" : "expression too complex");

                alloc->failed = true;

                return alloc;
            }

            at = next;
            next += width;
        }

        alloc->slots[v] = at;
    }

    alloc->frame_slots = next;

    return alloc;
}

unsigned int regalloc_slot_of(const RegAlloc *alloc, MIRValueId value) {
    if (mir_value_is_none(value) || value.id >= alloc->value_count) {
        return REGALLOC_NO_SLOT;
    }

    return alloc->slots[value.id];
}
