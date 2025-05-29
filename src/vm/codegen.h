#ifndef GAB_CODEGEN_H
#define GAB_CODEGEN_H

#include "ast/ast.h"
#include "vm/chunk.h"
#include "vm/vm.h"

Chunk *codegen_generate(ASTScript *ast, ValueList *global_data, FuncProtoList *global_funcs);

#endif
