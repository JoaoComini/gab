#ifndef GAB_AST_RESOLVE_H
#define GAB_AST_RESOLVE_H

#include "ast/ast.h"
#include "ast/facts.h"
#include "ast/pending.h"
#include "diagnostics.h"
#include "scope.h"

#include <stdbool.h>

/* Everything resolving a module concluded. Held here rather than on the nodes, so no stage before
 * resolution can read a fact and none after it can miss one. */
typedef struct ResolvedModule {
    ASTModule *module;

    /* What another module importing this one names it through, which outlives this compilation. */
    Module *declared;

    Facts facts;
    PendingBodies work;

    TypeRegistry *registry;
    FunctionRegistry *functions;
} ResolvedModule;

/* What a module is granted beyond what a program may declare. */
typedef struct {
    /* The intrinsics and the methods on primitives that the prelude declares and a program may not,
     * which an interface restating them is read with. */
    bool intrinsics;
} ModulePrivileges;

/* What every module resolved in one compilation shares: a scope names things and does not own them,
 * so what they are is held here rather than reached through whichever scope is in hand. */
typedef struct {
    Arena *arena;
    StringPool *strings;

    TypeRegistry *types;
    FunctionRegistry *functions;

    /* Holds the names the language predeclares, and is what every module scope hangs off. */
    Scope *global;

    /* What this compilation has read, which is what an import in it may name. */
    ModuleMap *modules;

    Diagnostics *diagnostics;
} Resolver;

/* False where the module does not resolve, so what it concluded exists only once it holds together.
 * It declares into 'into', which the caller makes: a module of its own, or the global scope for the
 * prelude, whose names the language predeclares as it does a primitive's. */
bool resolve_module(const Resolver *resolver, ASTModule *module, Scope *into, ModulePrivileges privileges,
                    ResolvedModule **out);

#endif
