#ifndef GAB_LLVM_SYMBOL_H
#define GAB_LLVM_SYMBOL_H

#include "binding.h"
#include "memory/arena.h"

/* The symbol a function is linked by: its module, its owner, and what a specialization was given,
 * joined so two declarations sharing a name in different places do not collide. */
const char *llvm_symbol_of(Arena *arena, const Function *function);

#endif
