#ifndef GAB_MIR_BORROWCK_H
#define GAB_MIR_BORROWCK_H

#include "decl.h"
#include "diagnostics.h"
#include "memory/arena.h"
#include "mir/mir.h"
#include "type/type_registry.h"

void mir_borrowck(Arena *arena, TypeRegistry *registry, MIRFunction *ir, Diagnostics *diagnostics,
                  Function *function, bool report);

#endif
