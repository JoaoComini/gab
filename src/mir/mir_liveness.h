#ifndef GAB_MIR_LIVENESS_H
#define GAB_MIR_LIVENESS_H

#include "memory/arena.h"
#include "mir/mir.h"

/* Where a value, or one field of it, is still going to be read; a borrow lives no longer than that. */
typedef struct {
    /* One bit per tracked place, per block, for what is live entering and leaving each block. */
    uint64_t *entry;
    uint64_t *exit;

    /* Scratch for a query that must walk back through a block, sized like one block's set. */
    uint64_t *scratch;

    /* Where each value's field bits begin, and how many it has; a value's own bit sits first. */
    size_t *value_base;
    size_t *value_fields;

    size_t place_count;

    size_t words_per_block;
    size_t block_count;
    size_t value_count;

    const MIRFunction *ir;
} Liveness;

Liveness *mir_liveness_compute(Arena *arena, const MIRFunction *ir);

bool mir_live_on_entry(const Liveness *liveness, MIRBlockId block, MIRValueId value);
bool mir_live_on_exit(const Liveness *liveness, MIRBlockId block, MIRValueId value);

/* Whether a value, or one field of it, is read after the given instruction; a value's lifetime
 * ends where it is not. Pass MIR_WHOLE_VALUE to ask about the value rather than one of its fields. */
bool mir_live_after(const Liveness *liveness, MIRBlockId block, size_t index, MIRValueId value,
                    MIRFieldId field);

/* The live set just after each instruction of a block, reported from its exit backwards, so a pass
 * wanting every point pays for one walk rather than one walk per point. */
void mir_live_walk_block(const Liveness *liveness, MIRBlockId block,
                         void (*visit)(void *context, const uint64_t *live), void *context);

/* Whether a value, or any field of it, is set in a live set 'mir_live_walk_block' reported. */
bool mir_live_set_holds(const Liveness *liveness, const uint64_t *set, MIRValueId value);

#endif
