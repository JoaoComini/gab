#include "std/std.h"

#include "core/core.h"

void std_register_all(VM *vm) {
    core_register_str(vm);

    std_register_string(vm);
    std_register_vec(vm);
}
