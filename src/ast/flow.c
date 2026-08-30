#include "ast/flow.h"

#define FLOW_INITIAL_CAPACITY 16

void flow_init(Flow *flow, Arena *arena) {
    flow->arena = arena;
    flow->slots = flow_map_create_alloc(arena_allocator(arena), FLOW_INITIAL_CAPACITY);
    flow->unreachable = false;
}

FlowSlot flow_get(const Flow *flow, Binding *binding) {
    FlowSlot *found = flow_map_lookup(flow->slots, binding);

    if (found) {
        return *found;
    }

    return (FlowSlot){.init = FLOW_UNREACHED, .inner_depth = 0, .written_fields = 0};
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
    } else {
        init = FLOW_UNINIT;
    }

    return (FlowSlot){
        .init = init,
        .inner_depth = a.inner_depth > b.inner_depth ? a.inner_depth : b.inner_depth,

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

        if (other.init != entry->value.init || other.inner_depth != entry->value.inner_depth ||
            other.written_fields != entry->value.written_fields) {
            return false;
        }
    }

    flow_for_each(b->slots, entry) {
        FlowSlot other = flow_get(a, entry->key);

        if (other.init != entry->value.init || other.inner_depth != entry->value.inner_depth ||
            other.written_fields != entry->value.written_fields) {
            return false;
        }
    }

    return true;
}
