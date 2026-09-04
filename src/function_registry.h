#ifndef GAB_FUNCTION_REGISTRY_H
#define GAB_FUNCTION_REGISTRY_H

#include "arena.h"
#include "binding.h"
#include "type/type.h"
#include "type/type_registry.h"

typedef struct FunctionRegistry FunctionRegistry;

FunctionRegistry *function_registry_create(Arena *arena, TypeRegistry *types);

void function_registry_destroy(FunctionRegistry *registry);

/* The instantiation of 'generic' for these arguments, made once and returned to every later caller. */
Function *function_registry_specialize(FunctionRegistry *registry, Function *generic, const TypeArg *args,
                                       size_t arg_count);

#endif
