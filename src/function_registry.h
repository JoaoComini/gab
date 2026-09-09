#ifndef GAB_FUNCTION_REGISTRY_H
#define GAB_FUNCTION_REGISTRY_H

#include "binding.h"
#include "memory/arena.h"
#include "type/type.h"
#include "type/type_registry.h"

typedef struct FunctionRegistry FunctionRegistry;

FunctionRegistry *function_registry_create(Arena *arena, TypeRegistry *types);

void function_registry_destroy(FunctionRegistry *registry);

/* False where 'type' already declares a method of this name, which it may do only once. */
bool function_registry_declare_owned(FunctionRegistry *registry, const Type *type, Function *function);

/* The method 'type' owns under this name, as it was declared and without specializing it. */
Function *function_registry_find_owned(FunctionRegistry *registry, const Type *type, const String *name);

/* The method 'type' owns under this name, specialized for its arguments where it has any. */
Function *function_registry_owned_for(FunctionRegistry *registry, const Type *type, const String *name);

/* True when a declaration's signature names no type parameter, so every instantiation shares it. */
bool function_registry_owned_is_shared(const Function *declaration, const Type *type);

/* What this type runs as it ends, or NULL where it declares no ending of its own. */
Function *function_registry_destructor(FunctionRegistry *registry, const Type *type);

/* The instantiation of 'decl' for these arguments, made once and returned to every later caller. */
Function *function_registry_instance(FunctionRegistry *registry, const FuncDecl *decl, const TypeArg *args,
                                     size_t arg_count);

#endif
