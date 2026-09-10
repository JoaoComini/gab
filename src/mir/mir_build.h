#ifndef GAB_MIR_BUILD_H
#define GAB_MIR_BUILD_H

#include "diagnostics.h"
#include "memory/arena.h"
#include "mir/mir_module.h"

struct ResolvedModule;

bool mir_build(Arena *arena, struct ResolvedModule *resolved, MIRModule *imported, MIRModule **out,
               Diagnostics *diagnostics);

#endif
