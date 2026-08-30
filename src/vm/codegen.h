#ifndef GAB_CODEGEN_H
#define GAB_CODEGEN_H

#include "arena.h"
#include "ast/ast.h"
#include "diagnostics.h"
#include "string/string_pool.h"
#include "type/type_registry.h"
#include "vm/chunk.h"
#include "vm/link.h"
#include "vm/opcode.h"

Unit *codegen_generate(ASTUnit *ast, Arena *arena, StringPool *strings, TypeRegistry *registry,
                       Diagnostics *diagnostics);

#endif
