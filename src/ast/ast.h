#ifndef GAB_AST_H
#define GAB_AST_H

#include "ast/stmt.h"
#include "diagnostics.h"
#include "scope.h"
#include "string/string_ref.h"
#include "util/list.h"

#include <stdbool.h>
#include <stddef.h>

// A module this unit may name. The span is where it was imported, so a module
// that turns out not to exist is reported against the line that asked for it.
typedef struct {
    StringRef name;
    Span span;
} ASTImport;

#define ast_import_list_item_free(item) ((void)(item))
GAB_LIST(ASTImportList, ast_import_list, ASTImport)

typedef struct ASTScript {
    ASTStmtList statements;

    // The unit's 'module' directive, which every unit has. The span is kept for
    // diagnostics about the directive itself.
    StringRef module_name;
    Span module_span;

    // The modules this unit declared it would name. A qualified reference to
    // anything else is an error, which is what makes the dependency between two
    // units something written down rather than discovered.
    ASTImportList imports;
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
