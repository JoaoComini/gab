#ifndef GAB_INTERP_H
#define GAB_INTERP_H

#include "vm/link.h"
#include "vm/vm.h"

#include <stddef.h>

VmRunStatus interp_run_top_level(VM *vm, const FuncPrototype *top_level);

VmRunStatus interp_run_frame(VM *vm, const FuncPrototype *proto, size_t base, unsigned int dest);

VmRunStatus interp_run_extern(VM *vm, const ExternProto *proto, size_t base);

bool vm_call_extern(VM *vm, const ExternProto *proto, size_t base);

void vm_fail(VM *vm, VmRunStatus status, const char *message);

bool vm_reserve_stack(const VM *vm, size_t needed);

size_t vm_live_stack_end(const VM *vm);

#endif
