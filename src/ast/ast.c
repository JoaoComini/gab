#include "ast.h"

ASTUnit *ast_unit_create(Arena *arena) {
    ASTUnit *unit = arena_alloc(arena, sizeof(ASTUnit));
    unit->arena = arena;
    unit->statements = ast_stmt_list_create(arena_allocator(arena));
    unit->instances = ast_stmt_list_create(arena_allocator(arena));
    unit->module_name = (StringRef){.data = NULL, .length = 0};
    unit->module_span = (Span){0};
    unit->imports = ast_import_list_create(arena_allocator(arena));

    return unit;
}

void ast_unit_add_statement(ASTUnit *unit, ASTStmt *stmt) { ast_stmt_list_add(&unit->statements, stmt); }
