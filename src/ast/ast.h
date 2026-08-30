#ifndef GAB_AST_H
#define GAB_AST_H

#include "ast/stmt.h"
#include "string/string_ref.h"
#include "util/list.h"

typedef struct {
    StringRef name;
    Span span;
} ASTImport;

#define ast_import_list_item_free(item) ((void)(item))
GAB_LIST(ASTImportList, ast_import_list, ASTImport)

typedef struct ASTUnit {
    ASTStmtList statements;

    ASTStmtList instances;

    StringRef module_name;
    Span module_span;

    ASTImportList imports;
} ASTUnit;

ASTUnit *ast_unit_create();
void ast_unit_add_statement(ASTUnit *unit, ASTStmt *stmt);
void ast_unit_destroy(ASTUnit *unit);

#endif
