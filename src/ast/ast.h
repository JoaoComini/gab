#ifndef GAB_AST_H
#define GAB_AST_H

#include "ast/stmt.h"
#include "diagnostics.h"
#include "scope.h"
#include "string/string_ref.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct ASTScript {
    ASTStmtList statements;

    // The unit's 'module' directive. 'module_name.data' is NULL when the unit
    // declared none, in which case its declarations belong to the root
    // namespace. The span is kept for diagnostics about the directive itself.
    StringRef module_name;
    Span module_span;
} ASTScript;

ASTScript *ast_script_create();
void ast_script_add_statement(ASTScript *script, ASTStmt *stmt);
// Returns false if any semantic error was reported. Resolution continues after
// an error so that multiple problems are reported in one pass.
// 'compile_arena' owns only what dies with the compile. Anything the resolver
// publishes into the global scope or the type registry is allocated from that
// scope's own arena instead, so it outlives the compile that produced it.
// How the resolver reaches another module's scope, for a 'Module::Type' name.
// The map is passed rather than the VM that owns it, so the resolver depends on
// a data structure and not on the runtime. NULL means no modules are visible; a
// name that misses is reported as an unknown type.
bool ast_script_resolve(Arena *compile_arena, ASTScript *script, Scope *global_scope,
                        ModuleScopeMap *module_scopes, Diagnostics *diagnostics);
void ast_script_destroy(ASTScript *script);

#endif
