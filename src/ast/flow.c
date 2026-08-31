#include "ast/flow.h"

#define FLOW_INITIAL_CAPACITY 16
#define FLOW_INITIAL_BORROW_CAPACITY 2

void flow_init(Flow *flow, Arena *arena) {
    flow->arena = arena;
    flow->slots = flow_map_create_alloc(arena_allocator(arena), FLOW_INITIAL_CAPACITY);
    flow->unreachable = false;
}

bool flow_slot_borrows_from(const FlowSlot *slot, const Binding *from) {
    for (size_t i = 0; i < slot->borrow_count; i++) {
        if (slot->borrows_from[i] == from) {
            return true;
        }
    }

    return false;
}

void flow_slot_add_source(Arena *arena, FlowSlot *slot, Binding *from) {
    if (!from || flow_slot_borrows_from(slot, from)) {
        return;
    }

    if (slot->borrow_count == slot->borrow_capacity) {
        size_t new_capacity =
            slot->borrow_capacity ? slot->borrow_capacity * 2 : FLOW_INITIAL_BORROW_CAPACITY;

        Binding **grown = arena_alloc(arena, new_capacity * sizeof(Binding *));

        for (size_t i = 0; i < slot->borrow_count; i++) {
            grown[i] = slot->borrows_from[i];
        }

        slot->borrows_from = grown;
        slot->borrow_capacity = new_capacity;
    }

    slot->borrows_from[slot->borrow_count++] = from;
}

void flow_slot_open_fields(FlowSlot *slot, Arena *arena, size_t count) {
    if (count == 0 || slot->field_count == count) {
        return;
    }

    slot->fields = arena_alloc(arena, count * sizeof(FlowSlot));
    slot->field_count = count;

    for (size_t i = 0; i < count; i++) {
        slot->fields[i] = (FlowSlot){.init = slot->init, .inner_depth = 0};
    }
}

FlowSlot flow_slot_flattened(Arena *arena, const FlowSlot *slot) {
    FlowSlot flat = *slot;

    flat.fields = NULL;
    flat.field_count = 0;
    flat.borrows_from = NULL;
    flat.borrow_count = 0;
    flat.borrow_capacity = 0;

    for (size_t i = 0; i < slot->borrow_count; i++) {
        flow_slot_add_source(arena, &flat, slot->borrows_from[i]);
    }

    for (size_t i = 0; i < slot->field_count; i++) {
        FlowSlot field = flow_slot_flattened(arena, &slot->fields[i]);

        if (field.inner_depth > flat.inner_depth) {
            flat.inner_depth = field.inner_depth;
        }

        /* Reading the whole value reads this field too, so its freed borrow is the value's. */
        if (flat.init == FLOW_INIT && field.init == FLOW_DANGLING) {
            flat.init = FLOW_DANGLING;
        }

        for (size_t j = 0; j < field.borrow_count; j++) {
            flow_slot_add_source(arena, &flat, field.borrows_from[j]);
        }
    }

    return flat;
}

FlowSlot flow_get(const Flow *flow, Binding *binding) {
    FlowSlot *found = flow_map_lookup(flow->slots, binding);

    if (found) {
        return *found;
    }

    return (FlowSlot){.init = FLOW_UNREACHED, .inner_depth = 0};
}

void flow_set(Flow *flow, Binding *binding, FlowSlot slot) {
    FlowSlot *found = flow_map_lookup(flow->slots, binding);

    if (found) {
        *found = slot;
        return;
    }

    flow_map_insert(flow->slots, binding, slot);
}

/* Field and source arrays are shared until written, so a slot entering a new flow takes its own copy. */
static FlowSlot slot_copy(Arena *arena, FlowSlot slot) {
    if (slot.borrow_capacity > 0) {
        Binding **borrows_from = arena_alloc(arena, slot.borrow_capacity * sizeof(Binding *));

        for (size_t i = 0; i < slot.borrow_count; i++) {
            borrows_from[i] = slot.borrows_from[i];
        }

        slot.borrows_from = borrows_from;
    }

    if (slot.field_count == 0) {
        return slot;
    }

    FlowSlot *fields = arena_alloc(arena, slot.field_count * sizeof(FlowSlot));

    for (size_t i = 0; i < slot.field_count; i++) {
        fields[i] = slot_copy(arena, slot.fields[i]);
    }

    slot.fields = fields;

    return slot;
}

void flow_copy(Flow *into, const Flow *from) {
    into->unreachable = from->unreachable;

    flow_map_init_alloc(into->slots, arena_allocator(into->arena), FLOW_INITIAL_CAPACITY);

    GAB_HASH_MAP_FOR_EACH(from->slots, entry) {
        flow_set(into, entry->key, slot_copy(into->arena, entry->value));
    }
}

static FlowSlot slot_merge(Arena *arena, FlowSlot a, FlowSlot b) {
    if (a.init == FLOW_UNREACHED) {
        return b;
    }

    if (b.init == FLOW_UNREACHED) {
        return a;
    }

    FlowInit init;

    if (a.init == FLOW_INIT && b.init == FLOW_INIT) {
        init = FLOW_INIT;
    } else if (a.init == FLOW_MOVED || b.init == FLOW_MOVED) {
        init = FLOW_MOVED;
    } else if (a.init == FLOW_DANGLING || b.init == FLOW_DANGLING) {
        init = FLOW_DANGLING;
    } else {
        init = FLOW_UNINIT;
    }

    FlowSlot merged = {
        .init = init,
        .inner_depth = a.inner_depth > b.inner_depth ? a.inner_depth : b.inner_depth,
    };

    /* A borrow reaching a join names whatever it named on either path. */
    for (size_t i = 0; i < a.borrow_count; i++) {
        flow_slot_add_source(arena, &merged, a.borrows_from[i]);
    }

    for (size_t i = 0; i < b.borrow_count; i++) {
        flow_slot_add_source(arena, &merged, b.borrows_from[i]);
    }

    /* A value tracked per field on one path is tracked per field at the join. */
    size_t field_count = a.field_count > b.field_count ? a.field_count : b.field_count;

    if (field_count > 0) {
        flow_slot_open_fields(&merged, arena, field_count);

        for (size_t i = 0; i < field_count; i++) {
            FlowSlot left = i < a.field_count ? a.fields[i] : (FlowSlot){.init = FLOW_UNREACHED};
            FlowSlot right = i < b.field_count ? b.fields[i] : (FlowSlot){.init = FLOW_UNREACHED};

            merged.fields[i] = slot_merge(arena, left, right);
        }
    }

    return merged;
}

void flow_merge(Flow *flow, const Flow *other) {
    if (other->unreachable) {
        return;
    }

    if (flow->unreachable) {
        flow_copy(flow, other);
        return;
    }

    GAB_HASH_MAP_FOR_EACH(other->slots, entry) {
        flow_set(flow, entry->key, slot_merge(flow->arena, flow_get(flow, entry->key), entry->value));
    }
}

static void slot_invalidate_borrows_of(FlowSlot *slot, Binding *freed) {
    for (size_t i = 0; i < slot->field_count; i++) {
        slot_invalidate_borrows_of(&slot->fields[i], freed);
    }

    if (slot->init != FLOW_INIT) {
        return;
    }

    /* A tracked field carries its own sources, so only what this slot names directly dangles it. */
    if (flow_slot_borrows_from(slot, freed)) {
        slot->init = FLOW_DANGLING;
    }
}

void flow_invalidate_borrows_of(Flow *flow, Binding *freed) {
    GAB_HASH_MAP_FOR_EACH(flow->slots, entry) { slot_invalidate_borrows_of(&entry->value, freed); }
}

static bool slot_equals(const FlowSlot *a, const FlowSlot *b) {
    if (a->init != b->init || a->inner_depth != b->inner_depth || a->borrow_count != b->borrow_count ||
        a->field_count != b->field_count) {
        return false;
    }

    for (size_t i = 0; i < a->borrow_count; i++) {
        if (!flow_slot_borrows_from(b, a->borrows_from[i])) {
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

/* Every slot of one side must equal the other's; a slot absent there reads as unreached. */
static bool flow_contained_by(const Flow *a, const Flow *b) {
    GAB_HASH_MAP_FOR_EACH(a->slots, entry) {
        FlowSlot other = flow_get(b, entry->key);

        if (!slot_equals(&entry->value, &other)) {
            return false;
        }
    }

    return true;
}

bool flow_equals(const Flow *a, const Flow *b) {
    if (a->unreachable != b->unreachable) {
        return false;
    }

    return flow_contained_by(a, b) && flow_contained_by(b, a);
}
