#ifndef GAB_CODEGEN_H
#define GAB_CODEGEN_H

#include "ast.h"
#include "vm/vm.h"

Chunk *codegen_generate(ASTScript *ast);

#endif
