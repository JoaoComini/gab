#ifndef GAB_COMPILE_H
#define GAB_COMPILE_H

#include "diagnostics.h"
#include "vm/link.h"

#include <stdbool.h>

typedef struct VM VM;

bool compile_unit(VM *vm, const char *source, FuncPrototype *out, Diagnostics *diagnostics);

void compile_and_run(VM *vm, const char *source);

#endif
