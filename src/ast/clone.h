#ifndef GAB_AST_CLONE_H
#define GAB_AST_CLONE_H

#include "arena.h"
#include "ast/stmt.h"
#include "ast/type_expr.h"

ASTStmt *ast_clone_stmt(Arena *arena, const ASTStmt *stmt);

/* Deep, unlike the sharing a clone within one unit does: for an AST outliving the arena it was parsed into.
 */
TypeExpr *ast_clone_type_expr(Arena *arena, const TypeExpr *expr);

#endif
