#ifndef GAB_AST_H
#define GAB_AST_H

#include "ast/stmt.h"
#include "scope.h"

#include <stddef.h>

typedef struct ASTScript {
    ASTStmtList statements;
} ASTScript;

ASTScript *ast_script_create();
void ast_script_add_statement(ASTScript *script, ASTStmt *stmt);
void ast_script_resolve(Arena *arena, ASTScript *script, Scope *global_scope);
void ast_script_destroy(ASTScript *script);

#endif
