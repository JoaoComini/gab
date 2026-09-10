#ifndef GAB_AST_RESOLVE_H
#define GAB_AST_RESOLVE_H

#include "ast/ast.h"
#include "ast/facts.h"
#include "ast/pending.h"
#include "diagnostics.h"
#include "scope.h"

#include <stdbool.h>

typedef struct ResolvedModule {
    ASTModule *module;

    Module *declared;

    Facts facts;
    PendingBodies work;

    TypeRegistry *registry;
    FunctionRegistry *functions;
} ResolvedModule;

typedef struct {
    bool intrinsics;
} ModulePrivileges;

typedef struct {
    Arena *arena;
    StringPool *strings;

    TypeRegistry *types;
    FunctionRegistry *functions;

    Scope *global;

    ModuleMap *modules;

    Module *core;

    Diagnostics *diagnostics;
} Resolver;

bool resolve_module(const Resolver *resolver, ASTModule *module, Scope *into, ModulePrivileges privileges,
                    ResolvedModule **out);

#endif
