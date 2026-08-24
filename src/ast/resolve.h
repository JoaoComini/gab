#ifndef GAB_AST_RESOLVE_H
#define GAB_AST_RESOLVE_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "scope.h"

#include <stdbool.h>

// Binds every name in a parsed unit to what it names and settles every type,
// annotating the tree in place. Runs the flow analysis over each body it
// resolves, so a unit that comes back true is one whose ownership and lifetime
// rules hold as well as its types.
//
// Returns false if any semantic error was reported. Resolution continues after
// an error so that multiple problems are reported in one pass.
//
// 'compile_arena' owns only what dies with the compile. Anything the resolver
// publishes into the global scope or the type registry is allocated from that
// scope's own arena instead, so it outlives the compile that produced it.
//
// 'module_scopes' is how the resolver reaches another module's scope, for a
// 'Module::Type' name. The map is passed rather than the VM that owns it, so
// the resolver depends on a data structure and not on the runtime. NULL means
// no modules are visible; a name that misses is reported as an unknown type.
bool resolve_unit(Arena *compile_arena, ASTUnit *unit, Scope *global_scope, ModuleScopeMap *module_scopes,
                  Diagnostics *diagnostics);

#endif
