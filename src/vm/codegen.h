#ifndef GAB_CODEGEN_H
#define GAB_CODEGEN_H

#include "memory/arena.h"
#include "ast/ast.h"
#include "diagnostics.h"
#include "mir/mir_module.h"
#include "string/string_pool.h"
#include "type/type_registry.h"
#include "vm/chunk.h"
#include "vm/link.h"
#include "vm/opcode.h"

bool codegen_generate(ASTUnit *ast, Arena *arena, StringPool *strings, TypeRegistry *registry,
                      const MIRModule *mir_unit, ObjectFile **out, Diagnostics *diagnostics);

#endif
