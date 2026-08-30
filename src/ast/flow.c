#include "ast/flow.h"

#define FLOW_INITIAL_CAPACITY 16

void flow_init(Flow *flow, Arena *arena) {
    flow->arena = arena;
    flow->slots = flow_map_create_alloc(arena_allocator(arena), FLOW_INITIAL_CAPACITY);
    flow->unreachable = false;
}

bool flow_slot_borrows_from(const FlowSlot *slot, const Binding *from) {
    if (slot->borrows_unknown) {
        return true;
    }

    for (size_t i = 0; i < slot->borrow_count; i++) {
        if (slot->borrows_from[i] == from) {
            return true;
        }
    }

    return false;
}

void flow_slot_add_source(FlowSlot *slot, Binding *from) {
    if (!from || flow_slot_borrows_from(slot, from)) {
        return;
    }

    if (slot->borrow_count == FLOW_MAX_BORROW_SOURCES) {
        slot->borrows_unknown = true;
        return;
    }

    slot->borrows_from[slot->borrow_count++] = from;
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

#define flow_for_each(map, entry)                                                                            \
    for (size_t _i = 0; _i < (map)->capacity; _i++)                                                          \
        for (FlowMapEntry *entry = (map)->buckets[_i]; entry; entry = entry->next)

void flow_copy(Flow *into, const Flow *from) {
    into->unreachable = from->unreachable;

    flow_map_init_alloc(into->slots, arena_allocator(into->arena), FLOW_INITIAL_CAPACITY);

    flow_for_each(from->slots, entry) { flow_set(into, entry->key, entry->value); }
}

static FlowSlot slot_merge(FlowSlot a, FlowSlot b) {
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
        .borrows_unknown = a.borrows_unknown || b.borrows_unknown,
    };

    /* A borrow reaching a join names whatever it named on either path. */
    for (size_t i = 0; i < a.borrow_count; i++) {
        flow_slot_add_source(&merged, a.borrows_from[i]);
    }

    for (size_t i = 0; i < b.borrow_count; i++) {
        flow_slot_add_source(&merged, b.borrows_from[i]);
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

    flow_for_each(other->slots, entry) {
        flow_set(flow, entry->key, slot_merge(flow_get(flow, entry->key), entry->value));
    }
}

void flow_invalidate_borrows_of(Flow *flow, Binding *freed) {
    flow_for_each(flow->slots, entry) {
        if (entry->value.init != FLOW_INIT || !flow_slot_borrows_from(&entry->value, freed)) {
            continue;
        }

        entry->value.init = FLOW_DANGLING;
    }
}

bool flow_equals(const Flow *a, const Flow *b) {
    if (a->unreachable != b->unreachable) {
        return false;
    }

    flow_for_each(a->slots, entry) {
        FlowSlot other = flow_get(b, entry->key);

        if (other.init != entry->value.init || other.inner_depth != entry->value.inner_depth ||
            other.borrow_count != entry->value.borrow_count ||
            other.borrows_unknown != entry->value.borrows_unknown) {
            return false;
        }

        for (size_t i = 0; i < entry->value.borrow_count; i++) {
            if (!flow_slot_borrows_from(&other, entry->value.borrows_from[i])) {
                return false;
            }
        }
    }

    flow_for_each(b->slots, entry) {
        FlowSlot other = flow_get(a, entry->key);

        if (other.init != entry->value.init || other.inner_depth != entry->value.inner_depth ||
            other.borrow_count != entry->value.borrow_count ||
            other.borrows_unknown != entry->value.borrows_unknown) {
            return false;
        }

        for (size_t i = 0; i < entry->value.borrow_count; i++) {
            if (!flow_slot_borrows_from(&other, entry->value.borrows_from[i])) {
                return false;
            }
        }
    }

    return true;
}
