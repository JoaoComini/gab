#ifndef GAB_FUNCTION_REGISTRY_H
#define GAB_FUNCTION_REGISTRY_H

#include "decl.h"
#include "memory/arena.h"
#include "type/type.h"
#include "type/type_registry.h"

typedef struct FunctionRegistry FunctionRegistry;

FunctionRegistry *function_registry_create(Arena *arena, TypeRegistry *types);

void function_registry_destroy(FunctionRegistry *registry);

bool function_registry_declare_owned(FunctionRegistry *registry, const Type *type, Function *function);

Function *function_registry_find_owned(FunctionRegistry *registry, const Type *type, const String *name);

Function *function_registry_owned_for(FunctionRegistry *registry, const Type *type, const String *name);

bool function_registry_owned_is_shared(const Function *declaration, const Type *type);

Function *function_registry_destructor(FunctionRegistry *registry, const Type *type);

Function *function_registry_instance(FunctionRegistry *registry, const FuncDecl *decl, const TypeArg *args,
                                     size_t arg_count);

#endif
