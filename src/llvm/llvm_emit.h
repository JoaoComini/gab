#ifndef GAB_LLVM_EMIT_H
#define GAB_LLVM_EMIT_H

#include "memory/arena.h"
#include "mir/mir.h"

/* A module under construction, which bodies are emitted into one at a time. */
typedef struct LLVMUnit LLVMUnit;

LLVMUnit *llvm_unit_open(Arena *arena);

void llvm_unit_add(LLVMUnit *unit, const MIRFunction *ir);

/* The module as IR text, arena-allocated and NUL-terminated. */
char *llvm_unit_text(LLVMUnit *unit);

/* Writes the module as a native object file, naming what went wrong where it fails. */
bool llvm_unit_write_object(LLVMUnit *unit, const char *path, const char **error);

void llvm_unit_close(LLVMUnit *unit);

/* Emits one body as LLVM IR text, arena-allocated and NUL-terminated. */
char *llvm_emit_function(Arena *arena, const MIRFunction *ir);

#endif
