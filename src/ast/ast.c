#include "ast.h"

#include <stdlib.h>

ASTUnit *ast_unit_create() {
    ASTUnit *unit = malloc(sizeof(ASTUnit));
    unit->statements = ast_stmt_list_create();
    unit->module_name = (StringRef){.data = NULL, .length = 0};
    unit->module_span = (Span){0};
    unit->imports = ast_import_list_create();

    return unit;
}

void ast_unit_destroy(ASTUnit *unit) {
    ast_stmt_list_free(&unit->statements);
    ast_import_list_free(&unit->imports);

    free(unit);
}

void ast_unit_add_statement(ASTUnit *unit, ASTStmt *stmt) { ast_stmt_list_add(&unit->statements, stmt); }
