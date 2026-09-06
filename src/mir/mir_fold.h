#ifndef GAB_MIR_FOLD_H
#define GAB_MIR_FOLD_H

#include "memory/arena.h"
#include "mir/mir.h"

/* Folds an operation on constants into the constant it yields, leaving what traps for the VM to reach. */
void mir_fold(Arena *arena, MIRFunction *ir);

#endif
