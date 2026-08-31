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

typedef struct FlowSlot {
    FlowInit init;

    int inner_depth;

    /* The slots this one borrows from, so freeing any of their objects marks this one dangling. */
    Binding **borrows_from;
    size_t borrow_count;
    size_t borrow_capacity;

    /* What each field holds, so reading one names its own sources rather than the whole struct's. */
    struct FlowSlot *fields;
    size_t field_count;
} FlowSlot;

/* Records 'from' as a slot this borrow names, growing the source array on the given arena as needed. */
void flow_slot_add_source(Arena *arena, FlowSlot *slot, Binding *from);
bool flow_slot_borrows_from(const FlowSlot *slot, const Binding *from);

/* Gives a slot room for 'count' fields, so each can be tracked apart from the others. */
void flow_slot_open_fields(FlowSlot *slot, Arena *arena, size_t count);

/* Folds every field into the slot itself, for a read that names the whole value. */
FlowSlot flow_slot_flattened(Arena *arena, const FlowSlot *slot);

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
