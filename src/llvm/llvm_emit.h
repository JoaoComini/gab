#ifndef GAB_LLVM_EMIT_H
#define GAB_LLVM_EMIT_H

#include "memory/arena.h"
#include "mir/mir.h"

/* Emits one body as LLVM IR text, arena-allocated and NUL-terminated. */
char *llvm_emit_function(Arena *arena, const MIRFunction *ir);

#endif
