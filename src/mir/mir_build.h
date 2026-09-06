#ifndef GAB_MIR_BUILD_H
#define GAB_MIR_BUILD_H

#include "memory/arena.h"
#include "diagnostics.h"
#include "mir/mir_module.h"

struct ResolvedUnit;

/* Lowers each body, substitutes the instances its calls named, and reports on moves and borrows. */
bool mir_build(Arena *arena, struct ResolvedUnit *resolved, MIRModule **out, Diagnostics *diagnostics);

#endif
