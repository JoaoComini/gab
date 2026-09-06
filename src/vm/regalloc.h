#ifndef GAB_REGALLOC_H
#define GAB_REGALLOC_H

#include "memory/arena.h"
#include "diagnostics.h"
#include "mir/mir.h"

#define REGALLOC_NO_SLOT ((unsigned int)-1)

/* Which frame slot holds each value, and how many the frame takes. */
typedef struct {
    unsigned int *slots;
    size_t value_count;

    unsigned int frame_slots;

    bool failed;
} RegAlloc;

/* Assigns a frame slot to every value, reusing what a dead one leaves behind. */
RegAlloc *regalloc_run(Arena *arena, MIRFunction *ir, Diagnostics *diagnostics);

unsigned int regalloc_slot_of(const RegAlloc *alloc, MIRValueId value);

#endif
