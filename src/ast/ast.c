#include "ast.h"

ASTModule *ast_module_create(Arena *arena) {
    ASTModule *module = arena_alloc(arena, sizeof(ASTModule));
    module->arena = arena;
    module->files = ast_file_list_create(arena_allocator(arena));
    module->name = NULL;

    return module;
}

ASTFile *ast_file_create(Arena *arena) {
    ASTFile *file = arena_alloc(arena, sizeof(ASTFile));
    file->statements = ast_stmt_list_create(arena_allocator(arena));
    file->imports = ast_import_list_create(arena_allocator(arena));
    file->module_name = NULL;

    return file;
}

void ast_module_add_file(ASTModule *module, ASTFile *file) { ast_file_list_add(&module->files, file); }

void ast_file_add_statement(ASTFile *file, ASTStmt *stmt) { ast_stmt_list_add(&file->statements, stmt); }

ASTStmtList *ast_module_statements(ASTModule *module) {
    if (module->files.size == 0) {
        ast_module_add_file(module, ast_file_create(module->arena));
    }

    return &module->files.data[0]->statements;
}
