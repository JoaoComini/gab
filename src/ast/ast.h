#ifndef GAB_AST_H
#define GAB_AST_H

#include "ast/stmt.h"
#include "diagnostics.h"
#include "scope.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct ASTScript {
    ASTStmtList statements;
} ASTScript;

ASTScript *ast_script_create();
void ast_script_add_statement(ASTScript *script, ASTStmt *stmt);
// Returns false if any semantic error was reported. Resolution continues after
// an error so that multiple problems are reported in one pass.
bool ast_script_resolve(Arena *arena, ASTScript *script, Scope *global_scope, Diagnostics *diagnostics);
void ast_script_destroy(ASTScript *script);

#endif
