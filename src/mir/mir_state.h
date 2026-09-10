#ifndef GAB_MIR_STATE_H
#define GAB_MIR_STATE_H

#include "memory/arena.h"
#include "mir/mir.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MIR_SLOT_UNREACHED = 0,
    MIR_SLOT_UNINIT,

    MIR_SLOT_MOVED,
    MIR_SLOT_DANGLING,
    MIR_SLOT_INIT,
} MIRSlotInit;

typedef struct MIRSlot {
    MIRSlotInit init;

    MIRValueId *borrows;
    size_t borrow_count;
    size_t borrow_capacity;

    struct MIRSlot *fields;
    size_t field_count;
} MIRSlot;

void mir_slot_add_borrow(Arena *arena, MIRSlot *slot, MIRValueId from);
bool mir_slot_borrows_from(const MIRSlot *slot, MIRValueId from);

void mir_slot_open_fields(MIRSlot *slot, Arena *arena, size_t count);

void mir_slot_set_all(MIRSlot *slot, MIRSlotInit init);

MIRSlot mir_slot_flattened(Arena *arena, const MIRSlot *slot);

#define mir_state_map_hash(key) ((size_t)(key).id * 2654435761u)
#define mir_state_map_key_equals(key, other) (key).id == (other).id

GAB_HASH_MAP(MIRStateMap, mir_state_map, MIRValueId, MIRSlot)

typedef struct {
    MIRStateMap *slots;
    Arena *arena;

    bool unreachable;
} MIRState;

void mir_state_init(MIRState *state, Arena *arena);

MIRSlot mir_state_get(const MIRState *state, MIRValueId value);
void mir_state_set(MIRState *state, MIRValueId value, MIRSlot slot);

void mir_state_copy(MIRState *into, const MIRState *from);
void mir_state_merge(MIRState *state, const MIRState *other);

void mir_state_invalidate_borrows_of(MIRState *state, MIRValueId dropped);

bool mir_state_equals(const MIRState *a, const MIRState *b);

#endif
