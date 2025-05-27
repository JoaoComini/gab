#ifndef GAB_CODEGEN_H
#define GAB_CODEGEN_H

#include "ast/ast.h"
#include "vm/chunk.h"

Chunk *codegen_generate(ASTScript *ast);

#endif
