#ifndef GAB_LLVM_SYMBOL_H
#define GAB_LLVM_SYMBOL_H

#include "decl.h"
#include "memory/arena.h"

const char *llvm_symbol_of(Arena *arena, const Function *function);

const char *llvm_type_symbol(Arena *arena, const Type *type);

#endif
