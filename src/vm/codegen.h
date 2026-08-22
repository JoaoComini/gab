#ifndef GAB_CODEGEN_H
#define GAB_CODEGEN_H

#include "arena.h"
#include "ast/ast.h"
#include "diagnostics.h"
#include "string/string_pool.h"
#include "vm/chunk.h"
#include "vm/link.h"
#include "vm/opcode.h"

// Generates a unit from a resolved AST. Returns NULL if generation failed; see
// the diagnostics sink.
//
// Nothing about a VM reaches here. The unit numbers its prototypes and types
// from zero and records every operand that will need rebasing, so what comes
// back is a self-contained thing a VM either links whole or never sees.
//
// 'arena' is where the unit's prototypes are allocated. It must outlive any VM
// the unit is linked into, because a frame addresses its prototype for as long
// as it runs.
Unit *codegen_generate(ASTScript *ast, Arena *arena, StringPool *strings, Diagnostics *diagnostics);

#endif
