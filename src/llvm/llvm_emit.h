#ifndef GAB_LLVM_EMIT_H
#define GAB_LLVM_EMIT_H

#include "memory/arena.h"
#include "mir/mir.h"

/* A module under construction, which bodies are emitted into one at a time. */
typedef struct LLVMUnit LLVMUnit;

LLVMUnit *llvm_unit_open(Arena *arena);

void llvm_unit_add(LLVMUnit *unit, const MIRFunction *ir);

/* States that this unit is the named interface: a byte the linker resolves against its importers. */
void llvm_unit_declares(LLVMUnit *unit, const char *symbol);

/* States that this unit was compiled against one, which links only where that interface defined it. */
void llvm_unit_requires(LLVMUnit *unit, const char *symbol);

/* The module as IR text, arena-allocated and NUL-terminated. */
char *llvm_unit_text(LLVMUnit *unit);

/* Writes the module as a native object file, naming what went wrong where it fails. */
bool llvm_unit_write_object(LLVMUnit *unit, const char *path, const char **error);

void llvm_unit_close(LLVMUnit *unit);

/* Emits one body as LLVM IR text, arena-allocated and NUL-terminated. */
char *llvm_emit_function(Arena *arena, const MIRFunction *ir);

#endif
