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

    /* Where this module's declarations landed, which is what another module names it through. */
    Scope *scope;

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

    /* Declares into the global scope rather than a scope of its own: the prelude is the language's own
     * vocabulary, so its names are reached the way a primitive's are, without an import. */
    bool global;
} ModulePrivileges;

/* False where the module does not resolve, so what it concluded exists only once it holds together. */
bool resolve_module(Arena *compile_arena, ASTModule *module, Scope *global_scope,
                    ModuleScopeMap *module_scopes, ModulePrivileges privileges, ResolvedModule **out,
                    Diagnostics *diagnostics);

#endif
