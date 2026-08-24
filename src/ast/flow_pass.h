#ifndef GAB_AST_FLOW_PASS_H
#define GAB_AST_FLOW_PASS_H

#include "arena.h"
#include "ast/stmt.h"
#include "diagnostics.h"

// Runs the flow analysis over one resolved function body and reports what it
// finds: a slot read after being moved out of, a pointer read before it holds
// anything, an owning field reached through before it is written, and a
// pointer stored somewhere that outlives what it points at.
//
// Separate from resolution because the two want opposite things from the tree.
// Resolution declares symbols and enters scopes, so it must run exactly once;
// this reads the symbols resolution bound and must run until the lattice stops
// changing, which around a loop is more than once. Running them together is
// what forced the old two-walk scheme, where the second walk's declarations
// had to be suppressed and its first pass's diagnostics thrown away.
//
// 'params' are the body's parameters, which start initialized: a caller
// supplied them, so nothing in the body may treat one as unwritten.
void flow_pass_run(Arena *arena, ASTStmt *body, Symbol **params, size_t param_count,
                   Diagnostics *diagnostics);

#endif
