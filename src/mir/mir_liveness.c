#include "mir/mir_liveness.h"

#include "type/type_registry.h"

#include <string.h>

/* Every value gets a bit for itself and one per field, so a field's own liveness is its own fact. */
static size_t place_index(const Liveness *liveness, MIRValueId value, MIRFieldId field) {
    size_t base = liveness->value_base[value.id];

    if (mir_field_is_whole(field) || field.id >= liveness->value_fields[value.id]) {
        return base;
    }

    return base + 1 + field.id;
}

static size_t word_of(size_t index) { return index / 64; }
static uint64_t bit_of(size_t index) { return (uint64_t)1 << (index % 64); }

static bool set_test(const uint64_t *set, size_t index) { return (set[word_of(index)] & bit_of(index)) != 0; }

static void set_add(uint64_t *set, size_t index) { set[word_of(index)] |= bit_of(index); }

static void set_remove(uint64_t *set, size_t index) { set[word_of(index)] &= ~bit_of(index); }

/* Reading a value whole reads every field it holds, so each of their facts becomes live too. */
static void read_place(const Liveness *liveness, uint64_t *set, MIRValueId value, MIRFieldId field) {
    if (mir_value_is_none(value) || value.id >= liveness->value_count) {
        return;
    }

    set_add(set, place_index(liveness, value, field));

    if (!mir_field_is_whole(field)) {
        return;
    }

    for (size_t i = 0; i < liveness->value_fields[value.id]; i++) {
        set_add(set, place_index(liveness, value, (MIRFieldId){(uint32_t)i}));
    }
}

/* Writing a place ends what it held; writing a value whole ends every field with it. */
static void write_place(const Liveness *liveness, uint64_t *set, MIRValueId value, MIRFieldId field) {
    if (mir_value_is_none(value) || value.id >= liveness->value_count) {
        return;
    }

    set_remove(set, place_index(liveness, value, field));

    if (!mir_field_is_whole(field)) {
        return;
    }

    for (size_t i = 0; i < liveness->value_fields[value.id]; i++) {
        set_remove(set, place_index(liveness, value, (MIRFieldId){(uint32_t)i}));
    }
}

/* The field a place names at its root, or the whole value where it names none. */
static MIRFieldId leading_field(const Place *place) {
    if (place->projection_count == 0) {
        return MIR_WHOLE_VALUE;
    }

    return place->projections[0].kind == PROJ_FIELD ? place->projections[0].field : MIR_WHOLE_VALUE;
}

/* A place reads every value its path names, since the address it forms depends on each of them. */
static void place_reads(const Liveness *liveness, const Place *place, uint64_t *set) {
    read_place(liveness, set, place->base, leading_field(place));

    for (size_t i = 0; i < place->projection_count; i++) {
        if (place->projections[i].kind == PROJ_INDEX) {
            read_place(liveness, set, place->projections[i].index, MIR_WHOLE_VALUE);
        }
    }
}

/* Walks one instruction backwards: what it writes stops being live, what it reads starts. */
static void step_backwards(const Liveness *liveness, const MIRInst *inst, uint64_t *set) {
    if (!mir_value_is_none(inst->result)) {
        write_place(liveness, set, inst->result, MIR_WHOLE_VALUE);
    }

    /* A store writes the place it names rather than reading it, ending what that place held. */
    if (inst->op == MIR_STORE) {
        MIRFieldId written = leading_field(&inst->place);

        write_place(liveness, set, inst->place.base, written);

        /* The address a deeper path forms is still read, as is any index along it. */
        if (inst->place.projection_count > 1) {
            read_place(liveness, set, inst->place.base, leading_field(&inst->place));
        }

        for (size_t i = 0; i < inst->place.projection_count; i++) {
            if (inst->place.projections[i].kind == PROJ_INDEX) {
                read_place(liveness, set, inst->place.projections[i].index, MIR_WHOLE_VALUE);
            }
        }
    } else if (inst->op == MIR_LOAD || inst->op == MIR_REF || inst->op == MIR_DROP) {
        place_reads(liveness, &inst->place, set);
    }

    /* Storage ending is not a read: what the local held is gone rather than wanted. */
    if (inst->op == MIR_STORAGE_LIVE || inst->op == MIR_STORAGE_DEAD) {
        write_place(liveness, set, inst->place.base, MIR_WHOLE_VALUE);
    }

    for (size_t i = 0; i < inst->arg_count; i++) {
        read_place(liveness, set, mir_operand_as_value(inst->args[i]), MIR_WHOLE_VALUE);
    }
}

Liveness *mir_liveness_compute(Arena *arena, const MIRFunction *ir) {
    Liveness *liveness = arena_alloc(arena, sizeof(Liveness));

    *liveness = (Liveness){.block_count = ir->block_count, .value_count = ir->value_count, .ir = ir};

    liveness->value_base = arena_alloc(arena, (ir->value_count + 1) * sizeof(size_t));
    liveness->value_fields = arena_alloc(arena, (ir->value_count + 1) * sizeof(size_t));

    /* A value's own bit comes first, then one per field it holds, so a field is its own fact. */
    size_t places = 0;

    for (size_t i = 0; i < ir->value_count; i++) {
        const MIRValueInfo *info = &ir->values[i];

        size_t fields = 0;

        if (info->type && type_kind(info->type) == TYPE_STRUCT) {
            const TypeFields *held = type_registry_fields_of(ir->registry, info->type);

            fields = held ? held->count : 0;
        }

        liveness->value_base[i] = places;
        liveness->value_fields[i] = fields;

        places += 1 + fields;
    }

    liveness->place_count = places;

    size_t words = (places + 63) / 64;

    if (words == 0) {
        words = 1;
    }

    liveness->words_per_block = words;

    size_t total = words * ir->block_count;

    liveness->entry = arena_alloc(arena, total * sizeof(uint64_t));
    liveness->exit = arena_alloc(arena, total * sizeof(uint64_t));

    memset(liveness->entry, 0, total * sizeof(uint64_t));
    memset(liveness->exit, 0, total * sizeof(uint64_t));

    liveness->scratch = arena_alloc(arena, words * sizeof(uint64_t));

    uint64_t *working = arena_alloc(arena, words * sizeof(uint64_t));

    bool changed = true;

    while (changed) {
        changed = false;

        /* Liveness flows backwards, so the last block settles first and the walk runs in reverse. */
        for (size_t i = ir->block_count; i > 0; i--) {
            const MIRBlock *block = ir->blocks[i - 1];

            uint64_t *exit = liveness->exit + (i - 1) * words;

            MIRBlockId successors[2];
            size_t count = mir_block_successors(block, successors);

            for (size_t s = 0; s < count; s++) {
                const uint64_t *successor_entry = liveness->entry + successors[s].id * words;

                for (size_t w = 0; w < words; w++) {
                    uint64_t before = exit[w];

                    exit[w] |= successor_entry[w];

                    changed = changed || exit[w] != before;
                }
            }

            memcpy(working, exit, words * sizeof(uint64_t));

            for (size_t j = block->inst_count; j > 0; j--) {
                step_backwards(liveness, &block->insts[j - 1], working);
            }

            uint64_t *entry = liveness->entry + (i - 1) * words;

            for (size_t w = 0; w < words; w++) {
                uint64_t before = entry[w];

                entry[w] |= working[w];

                changed = changed || entry[w] != before;
            }
        }
    }

    return liveness;
}

/* A value is live where any field of it is, since holding one field holds the storage of all. */
static bool any_field_live(const Liveness *liveness, const uint64_t *set, MIRValueId value) {
    if (set_test(set, place_index(liveness, value, MIR_WHOLE_VALUE))) {
        return true;
    }

    for (size_t i = 0; i < liveness->value_fields[value.id]; i++) {
        if (set_test(set, place_index(liveness, value, (MIRFieldId){(uint32_t)i}))) {
            return true;
        }
    }

    return false;
}

bool mir_live_on_entry(const Liveness *liveness, MIRBlockId block, MIRValueId value) {
    if (mir_block_is_none(block) || mir_value_is_none(value) || value.id >= liveness->value_count) {
        return false;
    }

    return any_field_live(liveness, liveness->entry + block.id * liveness->words_per_block, value);
}

bool mir_live_on_exit(const Liveness *liveness, MIRBlockId block, MIRValueId value) {
    if (mir_block_is_none(block) || mir_value_is_none(value) || value.id >= liveness->value_count) {
        return false;
    }

    return any_field_live(liveness, liveness->exit + block.id * liveness->words_per_block, value);
}

void mir_live_walk_block(const Liveness *liveness, MIRBlockId block,
                         void (*visit)(void *context, const uint64_t *live), void *context) {
    const MIRBlock *data = mir_block_at(liveness->ir, block);

    if (!data) {
        return;
    }

    size_t words = liveness->words_per_block;
    uint64_t *working = liveness->scratch;

    memcpy(working, liveness->exit + block.id * words, words * sizeof(uint64_t));

    for (size_t j = data->inst_count; j > 0; j--) {
        visit(context, working);

        step_backwards(liveness, &data->insts[j - 1], working);
    }
}

bool mir_live_set_holds(const Liveness *liveness, const uint64_t *set, MIRValueId value) {
    if (mir_value_is_none(value) || value.id >= liveness->value_count) {
        return false;
    }

    return any_field_live(liveness, set, value);
}

bool mir_live_after(const Liveness *liveness, MIRBlockId block, size_t index, MIRValueId value,
                    MIRFieldId field) {
    if (mir_block_is_none(block) || mir_value_is_none(value) || value.id >= liveness->value_count) {
        return false;
    }

    const MIRBlock *data = mir_block_at(liveness->ir, block);

    if (!data) {
        return false;
    }

    size_t words = liveness->words_per_block;

    uint64_t *working = liveness->scratch;

    /* Walking back from the block's exit to just past 'index' says what is still read from there. */
    memcpy(working, liveness->exit + block.id * words, words * sizeof(uint64_t));

    for (size_t j = data->inst_count; j > index + 1; j--) {
        step_backwards(liveness, &data->insts[j - 1], working);
    }

    if (mir_field_is_whole(field)) {
        return any_field_live(liveness, working, value);
    }

    return set_test(working, place_index(liveness, value, field));
}
