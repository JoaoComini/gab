#ifndef GAB_MIR_INSTANTIATE_H
#define GAB_MIR_INSTANTIATE_H

#include "function_registry.h"
#include "memory/arena.h"
#include "mir/mir.h"
#include "type/type_registry.h"

MIRFunction *mir_instantiate(Arena *arena, TypeRegistry *registry, FunctionRegistry *functions,
                             const MIRFunction *generic, Function *instance, const TypeArg *args,
                             size_t arg_count);

#endif
