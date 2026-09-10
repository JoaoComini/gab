#ifndef GAB_LLVM_EMIT_H
#define GAB_LLVM_EMIT_H

#include "memory/arena.h"
#include "mir/mir.h"

typedef struct LLVMUnit LLVMUnit;

LLVMUnit *llvm_unit_open(Arena *arena);

void llvm_unit_add(LLVMUnit *unit, const MIRFunction *ir);

void llvm_unit_declares(LLVMUnit *unit, const char *symbol);

void llvm_unit_requires(LLVMUnit *unit, const char *symbol);

char *llvm_unit_text(LLVMUnit *unit);

bool llvm_unit_write_object(LLVMUnit *unit, const char *path, const char **error);

void llvm_unit_close(LLVMUnit *unit);

char *llvm_emit_function(Arena *arena, const MIRFunction *ir);

#endif
