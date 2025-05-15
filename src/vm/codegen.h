#ifndef RULE_CODEGEN_H
#define RULE_CODEGEN_H

#include "ast.h"
#include "vm/vm.h"

Chunk *codegen_generate(ASTScript *ast);

#endif
