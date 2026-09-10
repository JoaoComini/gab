#ifndef GAB_MIR_FOLD_H
#define GAB_MIR_FOLD_H

#include "memory/arena.h"
#include "mir/mir.h"

void mir_fold(Arena *arena, MIRFunction *ir);

#endif
