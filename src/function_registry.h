#ifndef GAB_FUNCTION_REGISTRY_H
#define GAB_FUNCTION_REGISTRY_H

#include "memory/arena.h"
#include "binding.h"
#include "type/type.h"
#include "type/type_registry.h"

typedef struct FunctionRegistry FunctionRegistry;

FunctionRegistry *function_registry_create(Arena *arena, TypeRegistry *types);

void function_registry_destroy(FunctionRegistry *registry);

/* The method 'type' owns under this name, specialized for its arguments where it has any. */
Function *function_registry_owned_for(FunctionRegistry *registry, TypeRegistry *types, const Type *type,
                                      const String *name);

/* The instantiation of 'generic' for these arguments, made once and returned to every later caller. */
Function *function_registry_specialize(FunctionRegistry *registry, Function *generic, const TypeArg *args,
                                       size_t arg_count);

#endif
