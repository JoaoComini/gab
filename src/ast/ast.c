#include "ast.h"

#include <stdlib.h>

ASTScript *ast_script_create() {
    ASTScript *script = malloc(sizeof(ASTScript));
    script->statements = ast_stmt_list_create();
    script->module_name = (StringRef){.data = NULL, .length = 0};
    script->module_span = (Span){0};
    script->imports = ast_import_list_create();

    return script;
}

void ast_script_destroy(ASTScript *script) {
    ast_stmt_list_free(&script->statements);
    ast_import_list_free(&script->imports);

    free(script);
}

void ast_script_add_statement(ASTScript *script, ASTStmt *stmt) {
    ast_stmt_list_add(&script->statements, stmt);
}
