#ifndef GAB_AST_FLOW_PASS_H
#define GAB_AST_FLOW_PASS_H

#include "arena.h"
#include "ast/stmt.h"
#include "diagnostics.h"

void flow_pass_run(Arena *arena, ASTStmt *body, Symbol **params, size_t param_count, const Type *return_type,
                   Diagnostics *diagnostics);

#endif
