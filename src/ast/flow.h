#ifndef GAB_AST_FLOW_H
#define GAB_AST_FLOW_H

#include "arena.h"
#include "symbol_table.h"
#include "util/hash_map.h"

#include <stdbool.h>

// Per-slot state at one point in the program, and the merge that joins two
// points into one. The resolver walks statements in source order carrying a
// Flow; where control rejoins -- after an if/else, around a loop's back-edge --
// the branches' states are merged rather than one of them being kept.
//
// Two facts are tracked together because both need the same merge and building
// the framework twice would be waste:
//
//   - the block depth of what a pointer names, which the lifetime rule compares
//     against its destination, and
//   - whether the slot holds a value at all, and if not, whether that is
//     because it was never given one or because it was moved out of.
//
// They merge in opposite directions, which is the whole reason a lattice is
// needed rather than a field. A depth merges to the deeper -- the
// shorter-lived pointee -- because a borrow is safe only if it is safe on every
// path that reaches its use. Initialized-ness merges to the *less* initialized,
// because a slot only definitely holds a value if it does on every such path.

typedef enum {
    // No path has reached this slot's declaration yet, so it has no state.
    // Distinct from UNINIT: the merge treats it as the identity, so joining an
    // unreached path with a reached one keeps the reached one's answer.
    FLOW_UNREACHED = 0,
    FLOW_UNINIT,

    // Initialized, then moved out of. Distinct from UNINIT because the slot
    // did hold something and the programmer said where it went, so the
    // diagnostic can say so too.
    FLOW_MOVED,
    FLOW_INIT,
} FlowInit;

typedef struct {
    FlowInit init;

    // The block depth of what this slot points at, or 0 for "outlives
    // everything" -- a heap object, a global, or a non-pointer.
    int pointee_depth;
} FlowSlot;

#define flow_map_hash(key) ((size_t)(key) >> 4)
#define flow_map_key_equals(key, other) key == other
#define flow_map_key_dup(key) key
#define flow_map_entry_free(key, value)

GAB_HASH_MAP(FlowMap, flow_map, Symbol *, FlowSlot)

typedef struct {
    FlowMap *slots;
    Arena *arena;

    // Set where control cannot fall through to the next statement: after a
    // 'return', a 'break', or a 'continue'. A merge with an unreachable state
    // keeps the other side untouched, which is what makes
    // 'if c { return } else { x = &local }' report against the else alone.
    bool unreachable;
} Flow;

void flow_init(Flow *flow, Arena *arena);

// The state of one slot. A slot nothing has written to reads as UNREACHED at
// depth 0, so a caller that never declared it sees the identity rather than a
// missing entry.
FlowSlot flow_get(const Flow *flow, Symbol *symbol);
void flow_set(Flow *flow, Symbol *symbol, FlowSlot slot);

// Copies 'from' into 'into', which must already be initialized. Used to fork a
// branch's state off the state at the branch point.
void flow_copy(Flow *into, const Flow *from);

// Joins 'other' into 'flow': depth to the deeper, init to the less initialized,
// per slot. An unreachable side contributes nothing.
void flow_merge(Flow *flow, const Flow *other);

// Whether the two agree on every slot either mentions, which is the fixpoint
// test a loop iterates to.
bool flow_equals(const Flow *a, const Flow *b);

#endif
