#ifndef GAB_AST_RESOLVE_H
#define GAB_AST_RESOLVE_H

#include "ast/ast.h"
#include "ast/facts.h"
#include "ast/pending.h"
#include "diagnostics.h"
#include "scope.h"

#include <stdbool.h>

/* Everything resolving a unit concluded. Held here rather than on the nodes, so no stage before
 * resolution can read a fact and none after it can miss one. */
typedef struct ResolvedUnit {
    ASTUnit *unit;

    Facts facts;
    PendingBodies work;

    TypeRegistry *registry;
    FunctionRegistry *functions;
} ResolvedUnit;

/* False where the unit does not resolve, so what it concluded exists only once it holds together. */
bool resolve_unit(Arena *compile_arena, ASTUnit *unit, Scope *global_scope, ModuleScopeMap *module_scopes,
                  bool allow_primitive_impls, ResolvedUnit **out, Diagnostics *diagnostics);

#endif
