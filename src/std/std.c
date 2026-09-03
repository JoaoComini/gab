#include "std.h"

void std_register_all(GabVM *vm) {
    std_register_string(vm);
    std_register_vec(vm);
}
