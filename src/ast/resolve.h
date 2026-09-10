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

/* False where the module does not resolve, so what it concluded exists only once it holds together.
 * 'declares_intrinsics' admits the intrinsics and the methods on primitives that the prelude declares
 * and a program may not, which an interface restating them is read with. */
bool resolve_module(Arena *compile_arena, ASTModule *module, Scope *global_scope,
                    ModuleScopeMap *module_scopes, bool declares_intrinsics, ResolvedModule **out,
                    Diagnostics *diagnostics);

#endif
