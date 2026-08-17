#ifndef GAB_CODEGEN_H
#define GAB_CODEGEN_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "vm/chunk.h"
#include "vm/vm.h"

// Returns NULL if code generation failed; see the diagnostics sink.
Chunk *codegen_generate(ASTScript *ast, ValueList *global_data, FuncProtoList *global_funcs,
                        Diagnostics *diagnostics);

#endif
