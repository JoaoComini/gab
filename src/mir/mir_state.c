#include "mir/mir_state.h"

#define MIR_STATE_INITIAL_CAPACITY 16
#define MIR_SLOT_INITIAL_BORROW_CAPACITY 2

void mir_state_init(MIRState *state, Arena *arena) {
    state->arena = arena;
    state->slots = mir_state_map_create_alloc(arena_allocator(arena), MIR_STATE_INITIAL_CAPACITY);
    state->unreachable = false;
}

bool mir_slot_borrows_from(const MIRSlot *slot, MIRValueId from) {
    for (size_t i = 0; i < slot->borrow_count; i++) {
        if (slot->borrows[i].id == from.id) {
            return true;
        }
    }

    return false;
}

void mir_slot_add_borrow(Arena *arena, MIRSlot *slot, MIRValueId from) {
    if (mir_value_is_none(from) || mir_slot_borrows_from(slot, from)) {
        return;
    }

    if (slot->borrow_count == slot->borrow_capacity) {
        size_t capacity =
            slot->borrow_capacity ? slot->borrow_capacity * 2 : MIR_SLOT_INITIAL_BORROW_CAPACITY;

        MIRValueId *grown = arena_alloc(arena, capacity * sizeof(MIRValueId));

        for (size_t i = 0; i < slot->borrow_count; i++) {
            grown[i] = slot->borrows[i];
        }

        slot->borrows = grown;
        slot->borrow_capacity = capacity;
    }

    slot->borrows[slot->borrow_count++] = from;
}

void mir_slot_open_fields(MIRSlot *slot, Arena *arena, size_t count) {
    if (count == 0 || slot->field_count >= count) {
        return;
    }

    MIRSlot *grown = arena_alloc(arena, count * sizeof(MIRSlot));

    /* Fields already opened keep what they hold, since growing must not forget an earlier write. */
    for (size_t i = 0; i < slot->field_count; i++) {
        grown[i] = slot->fields[i];
    }

    for (size_t i = slot->field_count; i < count; i++) {
        grown[i] = (MIRSlot){.init = slot->init};
    }

    slot->fields = grown;
    slot->field_count = count;
}

MIRSlot mir_slot_flattened(Arena *arena, const MIRSlot *slot) {
    MIRSlot flat = *slot;

    flat.fields = NULL;
    flat.field_count = 0;
    flat.borrows = NULL;
    flat.borrow_count = 0;
    flat.borrow_capacity = 0;

    for (size_t i = 0; i < slot->borrow_count; i++) {
        mir_slot_add_borrow(arena, &flat, slot->borrows[i]);
    }

    for (size_t i = 0; i < slot->field_count; i++) {
        MIRSlot field = mir_slot_flattened(arena, &slot->fields[i]);

        /* Reading the whole value reads this field too, so its freed borrow is the value's. */
        if (flat.init == MIR_SLOT_INIT && field.init == MIR_SLOT_DANGLING) {
            flat.init = MIR_SLOT_DANGLING;
        }

        for (size_t j = 0; j < field.borrow_count; j++) {
            mir_slot_add_borrow(arena, &flat, field.borrows[j]);
        }
    }

    return flat;
}

MIRSlot mir_state_get(const MIRState *state, MIRValueId value) {
    MIRSlot *found = mir_state_map_lookup(state->slots, value);

    if (found) {
        return *found;
    }

    return (MIRSlot){.init = MIR_SLOT_UNREACHED};
}

void mir_state_set(MIRState *state, MIRValueId value, MIRSlot slot) {
    MIRSlot *found = mir_state_map_lookup(state->slots, value);

    if (found) {
        *found = slot;
        return;
    }

    mir_state_map_insert(state->slots, value, slot);
}

/* Field and borrow arrays are shared until written, so a slot entering a new state takes its own copy. */
static MIRSlot slot_copy(Arena *arena, MIRSlot slot) {
    if (slot.borrow_capacity > 0) {
        MIRValueId *borrows = arena_alloc(arena, slot.borrow_capacity * sizeof(MIRValueId));

        for (size_t i = 0; i < slot.borrow_count; i++) {
            borrows[i] = slot.borrows[i];
        }

        slot.borrows = borrows;
    }

    if (slot.field_count == 0) {
        return slot;
    }

    MIRSlot *fields = arena_alloc(arena, slot.field_count * sizeof(MIRSlot));

    for (size_t i = 0; i < slot.field_count; i++) {
        fields[i] = slot_copy(arena, slot.fields[i]);
    }

    slot.fields = fields;

    return slot;
}

void mir_state_copy(MIRState *into, const MIRState *from) {
    into->unreachable = from->unreachable;

    mir_state_map_init_alloc(into->slots, arena_allocator(into->arena), MIR_STATE_INITIAL_CAPACITY);

    GAB_HASH_MAP_FOR_EACH(from->slots, entry) {
        mir_state_map_insert(into->slots, entry->key, slot_copy(into->arena, entry->value));
    }
}

/* Two paths reaching a point agree on the worst each says: the least initialized, and every source. */
static MIRSlot slot_merge(Arena *arena, MIRSlot a, MIRSlot b) {
    if (a.init == MIR_SLOT_UNREACHED) {
        return slot_copy(arena, b);
    }

    if (b.init == MIR_SLOT_UNREACHED) {
        return slot_copy(arena, a);
    }

    MIRSlot merged = slot_copy(arena, a);

    if (b.init < merged.init) {
        merged.init = b.init;
    }

    for (size_t i = 0; i < b.borrow_count; i++) {
        mir_slot_add_borrow(arena, &merged, b.borrows[i]);
    }

    size_t fields = a.field_count > b.field_count ? a.field_count : b.field_count;

    if (fields > 0) {
        MIRSlot *grown = arena_alloc(arena, fields * sizeof(MIRSlot));

        for (size_t i = 0; i < fields; i++) {
            MIRSlot left = i < a.field_count ? a.fields[i] : (MIRSlot){.init = a.init};
            MIRSlot right = i < b.field_count ? b.fields[i] : (MIRSlot){.init = b.init};

            grown[i] = slot_merge(arena, left, right);
        }

        merged.fields = grown;
        merged.field_count = fields;
    }

    return merged;
}

void mir_state_merge(MIRState *state, const MIRState *other) {
    if (other->unreachable) {
        return;
    }

    if (state->unreachable) {
        mir_state_copy(state, other);
        return;
    }

    GAB_HASH_MAP_FOR_EACH(other->slots, entry) {
        mir_state_set(state, entry->key,
                      slot_merge(state->arena, mir_state_get(state, entry->key), entry->value));
    }
}

static void slot_invalidate_borrows_of(MIRSlot *slot, MIRValueId dropped) {
    for (size_t i = 0; i < slot->field_count; i++) {
        slot_invalidate_borrows_of(&slot->fields[i], dropped);
    }

    if (slot->init == MIR_SLOT_INIT && mir_slot_borrows_from(slot, dropped)) {
        slot->init = MIR_SLOT_DANGLING;
    }
}

void mir_state_invalidate_borrows_of(MIRState *state, MIRValueId dropped) {
    GAB_HASH_MAP_FOR_EACH(state->slots, entry) { slot_invalidate_borrows_of(&entry->value, dropped); }
}

static bool slot_equals(const MIRSlot *a, const MIRSlot *b) {
    if (a->init != b->init || a->borrow_count != b->borrow_count || a->field_count != b->field_count) {
        return false;
    }

    for (size_t i = 0; i < a->borrow_count; i++) {
        if (!mir_slot_borrows_from(b, a->borrows[i])) {
            return false;
        }
    }

    for (size_t i = 0; i < a->field_count; i++) {
        if (!slot_equals(&a->fields[i], &b->fields[i])) {
            return false;
        }
    }

    return true;
}

static bool state_contained_by(const MIRState *a, const MIRState *b) {
    GAB_HASH_MAP_FOR_EACH(a->slots, entry) {
        MIRSlot other = mir_state_get(b, entry->key);

        if (!slot_equals(&entry->value, &other)) {
            return false;
        }
    }

    return true;
}

bool mir_state_equals(const MIRState *a, const MIRState *b) {
    if (a->unreachable != b->unreachable) {
        return false;
    }

    return state_contained_by(a, b) && state_contained_by(b, a);
}

void mir_slot_set_all(MIRSlot *slot, MIRSlotInit init) {
    slot->init = init;

    for (size_t i = 0; i < slot->field_count; i++) {
        mir_slot_set_all(&slot->fields[i], init);
    }
}
