#ifndef GAB_AST_H
#define GAB_AST_H

#include "ast/stmt.h"
#include "string/string_ref.h"
#include "util/list.h"

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
void ast_script_destroy(ASTScript *script);

#endif
