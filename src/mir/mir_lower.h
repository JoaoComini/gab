#ifndef GAB_MIR_LOWER_H
#define GAB_MIR_LOWER_H

#include "ast/facts.h"
#include "ast/stmt.h"
#include "memory/arena.h"
#include "mir/mir.h"
#include "type/type_registry.h"

MIRFunction *mir_build_function(Arena *arena, TypeRegistry *registry, const Facts *facts, Function *function,
                                const ASTFieldList *params, ASTStmt *body);

#endif
