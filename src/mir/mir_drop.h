#ifndef GAB_MIR_DROP_H
#define GAB_MIR_DROP_H

#include "function_registry.h"
#include "memory/arena.h"
#include "mir/mir.h"
#include "type/type_registry.h"

void mir_drop_elaborate(Arena *arena, TypeRegistry *registry, FunctionRegistry *functions, MIRFunction *ir);

#endif
