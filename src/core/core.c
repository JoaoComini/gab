#include "core/core.h"

#include "core/slice.h"
#include "core/str.h"

void core_register_all(VM *vm) {
    /* 'str' reads as a slice of bytes, so what a slice declares must already be there. */
    core_register_slice(vm);
    core_register_str(vm);
}
