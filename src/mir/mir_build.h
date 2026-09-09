#ifndef GAB_MIR_BUILD_H
#define GAB_MIR_BUILD_H

#include "diagnostics.h"
#include "memory/arena.h"
#include "mir/mir_module.h"

struct ResolvedModule;

/* Lowers each body, substitutes the instances its calls named, and reports on moves and borrows. */
/* The bodies of generics an import declared, which a reader instantiates rather than links against: what
 * was compiled elsewhere was compiled for arguments that unit saw, and never for the reader's own. */
bool mir_build(Arena *arena, struct ResolvedModule *resolved, MIRModule *imported, MIRModule **out,
               Diagnostics *diagnostics);

#endif
