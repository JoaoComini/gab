#ifndef GAB_AST_FLOW_PASS_H
#define GAB_AST_FLOW_PASS_H

#include "arena.h"
#include "ast/stmt.h"
#include "binding.h"
#include "diagnostics.h"

void flow_pass_run(Arena *arena, TypeRegistry *registry, ASTStmt *body, Binding **params, size_t param_count,
                   const Type *return_type, Diagnostics *diagnostics, Function *function, bool report);

#endif
