#ifndef GAB_AST_CLONE_H
#define GAB_AST_CLONE_H

#include "arena.h"
#include "ast/stmt.h"

ASTStmt *ast_clone_stmt(Arena *arena, const ASTStmt *stmt);

#endif
