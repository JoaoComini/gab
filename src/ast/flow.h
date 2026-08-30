#ifndef GAB_AST_FLOW_H
#define GAB_AST_FLOW_H

#include "arena.h"
#include "symbol_table.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FLOW_UNREACHED = 0,
    FLOW_UNINIT,

    FLOW_MOVED,
    FLOW_INIT,
} FlowInit;

#define FLOW_MAX_FIELDS 64

typedef struct {
    FlowInit init;

    int inner_depth;

    uint64_t written_fields;
} FlowSlot;

#define flow_map_hash(key) ((size_t)(key) >> 4)
#define flow_map_key_equals(key, other) key == other
#define flow_map_key_dup(key) key
#define flow_map_entry_free(key, value)

GAB_HASH_MAP(FlowMap, flow_map, Symbol *, FlowSlot)

typedef struct {
    FlowMap *slots;
    Arena *arena;

    bool unreachable;
} Flow;

void flow_init(Flow *flow, Arena *arena);

FlowSlot flow_get(const Flow *flow, Symbol *symbol);
void flow_set(Flow *flow, Symbol *symbol, FlowSlot slot);

void flow_copy(Flow *into, const Flow *from);

void flow_merge(Flow *flow, const Flow *other);

bool flow_equals(const Flow *a, const Flow *b);

#endif
