#ifndef GAB_AST_H
#define GAB_AST_H

#include "ast/ident.h"
#include "ast/stmt.h"
#include "memory/arena.h"
#include "string/string_ref.h"
#include "util/list.h"

typedef struct {
    ASTIdent *name;
} ASTImport;

GAB_LIST(ASTImportList, ast_import_list, ASTImport)

typedef struct ASTFile {
    ASTStmtList statements;

    ASTImportList imports;

    ASTIdent *module_name;
} ASTFile;

GAB_LIST(ASTFileList, ast_file_list, ASTFile *)

typedef struct ASTModule {
    Arena *arena;

    ASTFileList files;

    ASTIdent *name;
} ASTModule;

ASTModule *ast_module_create(Arena *arena);

ASTFile *ast_file_create(Arena *arena);
void ast_module_add_file(ASTModule *module, ASTFile *file);

void ast_file_add_statement(ASTFile *file, ASTStmt *stmt);

ASTStmtList *ast_module_statements(ASTModule *module);

#endif
