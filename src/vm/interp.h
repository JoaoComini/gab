#ifndef GAB_INTERP_H
#define GAB_INTERP_H

#include "vm/link.h"
#include "vm/vm.h"

#include <stddef.h>

// Runs a unit's top level as frame zero, leaving its result in slot 0. Returns
// why the run stopped; vm->error carries the same status plus a message.
// Nothing is printed — reporting belongs to the caller.
VmRunStatus interp_run_top_level(VM *vm, const FuncPrototype *top_level);

// Pushes one frame and runs the interpreter until it unwinds. The result is
// left at the frame's own r0, which is the slot at base. The embedding API
// calls in through this; base is a byte offset into the stack, and the caller
// must already have placed the arguments in the parameter slots above it.
VmRunStatus interp_run_frame(VM *vm, const FuncPrototype *proto, size_t base, unsigned int dest);

// Runs an extern's host body against the block at 'base', for a host calling
// one directly. No frame is pushed and no bytecode runs: an extern has none,
// and its arguments are already laid out where a callee's would be.
VmRunStatus interp_run_extern(VM *vm, const FuncPrototype *proto, size_t base);

// Records why a run stopped, copying the message. The first failure wins: a
// later one is a consequence of unwinding, not an independent problem. An
// extern reports through this, which is why it is not private to the loop.
void vm_fail(VM *vm, VmRunStatus status, const char *message);

#endif
