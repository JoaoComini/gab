#ifndef GAB_MIR_LIVENESS_H
#define GAB_MIR_LIVENESS_H

#include "memory/arena.h"
#include "mir/mir.h"

typedef struct {
    uint64_t *entry;
    uint64_t *exit;

    uint64_t *scratch;

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

bool mir_live_after(const Liveness *liveness, MIRBlockId block, size_t index, MIRValueId value,
                    MIRFieldId field);

void mir_live_walk_block(const Liveness *liveness, MIRBlockId block,
                         void (*visit)(void *context, const uint64_t *live), void *context);

bool mir_live_set_holds(const Liveness *liveness, const uint64_t *set, MIRValueId value);

#endif
