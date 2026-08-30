#ifndef GAB_AST_FLOW_H
#define GAB_AST_FLOW_H

#include "arena.h"
#include "binding.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FLOW_UNREACHED = 0,
    FLOW_UNINIT,

    FLOW_MOVED,
    FLOW_DANGLING,
    FLOW_INIT,
} FlowInit;

#define FLOW_MAX_BORROW_SOURCES 4

typedef struct {
    FlowInit init;

    int inner_depth;

    /* The slots this one borrows from, so freeing any of their objects marks this one dangling. */
    Binding *borrows_from[FLOW_MAX_BORROW_SOURCES];
    size_t borrow_count;

    /* Set when more sources were named than fit: the slot then borrows from every slot, not from none. */
    bool borrows_unknown;
} FlowSlot;

/* Records 'from' as a slot this borrow names, if there is room and it is not already there. */
void flow_slot_add_source(FlowSlot *slot, Binding *from);
bool flow_slot_borrows_from(const FlowSlot *slot, const Binding *from);

#define flow_map_hash(key) ((size_t)(key) >> 4)
#define flow_map_key_equals(key, other) key == other
#define flow_map_key_dup(key) key
#define flow_map_entry_free(key, value)

GAB_HASH_MAP(FlowMap, flow_map, Binding *, FlowSlot)

typedef struct {
    FlowMap *slots;
    Arena *arena;

    bool unreachable;
} Flow;

void flow_init(Flow *flow, Arena *arena);

FlowSlot flow_get(const Flow *flow, Binding *binding);
void flow_set(Flow *flow, Binding *binding, FlowSlot slot);

void flow_copy(Flow *into, const Flow *from);

void flow_merge(Flow *flow, const Flow *other);

/* Marks every slot borrowing from 'freed' as dangling. */
void flow_invalidate_borrows_of(Flow *flow, Binding *freed);

bool flow_equals(const Flow *a, const Flow *b);

#endif
