#ifndef GAB_AST_RESOLVE_H
#define GAB_AST_RESOLVE_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "scope.h"

#include <stdbool.h>

bool resolve_unit(Arena *compile_arena, ASTUnit *unit, Scope *global_scope, ModuleScopeMap *module_scopes,
                  Diagnostics *diagnostics);

#endif
