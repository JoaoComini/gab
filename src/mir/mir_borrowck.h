#ifndef GAB_MIR_BORROWCK_H
#define GAB_MIR_BORROWCK_H

#include "memory/arena.h"
#include "decl.h"
#include "diagnostics.h"
#include "mir/mir.h"
#include "type/type_registry.h"

/* Checks that nothing is read after it stops holding a value, and that no borrow outlives what it names. */
void mir_borrowck(Arena *arena, TypeRegistry *registry, MIRFunction *ir, Diagnostics *diagnostics,
                  Function *function, bool report);

#endif
