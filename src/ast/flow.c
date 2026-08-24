#include "ast/flow.h"

#define FLOW_INITIAL_CAPACITY 16

void flow_init(Flow *flow, Arena *arena) {
    flow->arena = arena;
    flow->slots = flow_map_create_alloc(arena_allocator(arena), FLOW_INITIAL_CAPACITY);
    flow->unreachable = false;
}

FlowSlot flow_get(const Flow *flow, Symbol *symbol) {
    FlowSlot *found = flow_map_lookup(flow->slots, symbol);

    if (found) {
        return *found;
    }

    return (FlowSlot){.init = FLOW_UNREACHED, .pointee_depth = 0, .written_fields = 0};
}

void flow_set(Flow *flow, Symbol *symbol, FlowSlot slot) {
    FlowSlot *found = flow_map_lookup(flow->slots, symbol);

    if (found) {
        *found = slot;
        return;
    }

    flow_map_insert(flow->slots, symbol, slot);
}

// Walks every entry of a FlowMap. The map exposes buckets rather than an
// iterator, and three of the operations here need one.
#define flow_for_each(map, entry)                                                                            \
    for (size_t _i = 0; _i < (map)->capacity; _i++)                                                          \
        for (FlowMapEntry *entry = (map)->buckets[_i]; entry; entry = entry->next)

void flow_copy(Flow *into, const Flow *from) {
    into->unreachable = from->unreachable;

    flow_map_init_alloc(into->slots, arena_allocator(into->arena), FLOW_INITIAL_CAPACITY);

    flow_for_each(from->slots, entry) { flow_set(into, entry->key, entry->value); }
}

// A slot's two facts merge in opposite directions, each toward the answer that
// holds on every path: the deeper pointee, and the weaker initialized-ness.
static FlowSlot slot_merge(FlowSlot a, FlowSlot b) {
    if (a.init == FLOW_UNREACHED) {
        return b;
    }

    if (b.init == FLOW_UNREACHED) {
        return a;
    }

    // A slot is usable after the join only if it is usable on both paths, so
    // anything short of INIT on either side wins. Between the two unusable
    // answers MOVED is kept: a slot moved on one path and merely uninitialized
    // on the other is best reported as moved, which names where it went.
    FlowInit init;

    if (a.init == FLOW_INIT && b.init == FLOW_INIT) {
        init = FLOW_INIT;
    } else if (a.init == FLOW_MOVED || b.init == FLOW_MOVED) {
        init = FLOW_MOVED;
    } else {
        init = FLOW_UNINIT;
    }

    return (FlowSlot){
        .init = init,
        .pointee_depth = a.pointee_depth > b.pointee_depth ? a.pointee_depth : b.pointee_depth,

        // A field is written after the join only where both paths wrote it,
        // which is the intersection.
        .written_fields = a.written_fields & b.written_fields,
    };
}

void flow_merge(Flow *flow, const Flow *other) {
    if (other->unreachable) {
        return;
    }

    if (flow->unreachable) {
        flow_copy(flow, other);
        return;
    }

    // Every slot either side mentions, not just the ones both do: a slot
    // written on one path only still has to merge against the other path's
    // absent -- UNREACHED -- state.
    flow_for_each(other->slots, entry) {
        flow_set(flow, entry->key, slot_merge(flow_get(flow, entry->key), entry->value));
    }
}

bool flow_equals(const Flow *a, const Flow *b) {
    if (a->unreachable != b->unreachable) {
        return false;
    }

    flow_for_each(a->slots, entry) {
        FlowSlot other = flow_get(b, entry->key);

        if (other.init != entry->value.init || other.pointee_depth != entry->value.pointee_depth ||
            other.written_fields != entry->value.written_fields) {
            return false;
        }
    }

    flow_for_each(b->slots, entry) {
        FlowSlot other = flow_get(a, entry->key);

        if (other.init != entry->value.init || other.pointee_depth != entry->value.pointee_depth ||
            other.written_fields != entry->value.written_fields) {
            return false;
        }
    }

    return true;
}
