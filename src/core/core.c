#include "core/core.h"

#include "core/slice.h"
#include "core/str.h"

void core_register_all(VM *vm) {
    core_register_str(vm);
    core_register_slice(vm);
}
