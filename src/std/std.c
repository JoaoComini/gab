#include "std/std.h"

void std_register_all(VM *vm) {
    std_register_string(vm);
    std_register_vec(vm);
}
