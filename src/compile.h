#ifndef GAB_COMPILE_H
#define GAB_COMPILE_H

#include "diagnostics.h"
#include "vm/link.h"

#include <stdbool.h>

typedef struct VM VM;

// Drives lexer, parser, resolver and codegen over one unit of source, then
// links what comes out into the VM.
//
// Compiles without running, so a unit can be compiled once and run many
// times. Returns false and leaves 'out' untouched if any stage failed; the
// diagnostics say why. On success 'out' is the unit's top level, which the
// caller owns and must pass to func_proto_free; everything else the unit
// produced now belongs to the VM.
//
// Diagnostics are allocated from the VM's compile arena, which the next compile
// reclaims — so they stay readable until then, but not past it.
bool compile_unit(VM *vm, const char *source, FuncPrototype *out, Diagnostics *diagnostics);

// Compile, run, and discard, reporting any diagnostics to stderr. The
// convenience path for a caller with nothing to say about failure.
void compile_and_run(VM *vm, const char *source);

#endif
