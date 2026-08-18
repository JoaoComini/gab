#ifndef GAB_CODEGEN_H
#define GAB_CODEGEN_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "vm/chunk.h"
#include "vm/vm.h"

// Returns NULL if code generation failed; see the diagnostics sink.
// max_registers reports how many registers the top-level chunk addresses, so
// the caller can size its frame. Pass NULL if that is not needed.
Chunk *codegen_generate(ASTScript *ast, FuncProtoList *global_funcs, Diagnostics *diagnostics,
                        unsigned int *max_registers);

#endif
