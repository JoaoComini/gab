#ifndef GAB_LLVM_SYMBOL_H
#define GAB_LLVM_SYMBOL_H

#include "decl.h"
#include "memory/arena.h"

/* The symbol a function is linked by: its module, its owner, and what a specialization was given,
 * joined so two declarations sharing a name in different places do not collide. */
const char *llvm_symbol_of(Arena *arena, const Function *function);

/* A type's structural name, so a symbol derived from a type is the same in every build that emits it. */
const char *llvm_type_symbol(Arena *arena, const Type *type);

#endif
