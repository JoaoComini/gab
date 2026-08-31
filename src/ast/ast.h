#ifndef GAB_AST_H
#define GAB_AST_H

#include "arena.h"
#include "ast/stmt.h"
#include "string/string_ref.h"
#include "util/list.h"

typedef struct {
    StringRef name;
    Span span;
} ASTImport;

GAB_LIST_ALLOC(ASTImportList, ast_import_list, ASTImport)

typedef struct ASTUnit {
    Arena *arena;

    ASTStmtList statements;

    ASTStmtList instances;

    StringRef module_name;
    Span module_span;

    ASTImportList imports;
} ASTUnit;

ASTUnit *ast_unit_create(Arena *arena);
void ast_unit_add_statement(ASTUnit *unit, ASTStmt *stmt);

#endif
