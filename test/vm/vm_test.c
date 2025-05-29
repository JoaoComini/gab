#include "vm/vm.h"

#include <assert.h>

static void test_vm_execute() {
    VM *vm = vm_create();

    vm_execute(vm, "func execute(): bool {\n"
                   "let a = 2;\n"
                   "let b = 3;\n"
                   "let c = a + b * 5;\n"
                   "let d = (c - a) * ((b + 4) / (a + 1));\n"
                   "let e = d + (c * (a - b) + (b / (a + 2)));\n"
                   "let f = e - ((d + c) * (b - a));\n"
                   "let g = ((f + e) * (d - c)) / ((a + b) - (e / (d + 1)));\n"
                   "let h = g + f - e * (d + c - (b * a));\n"
                   "let i = (h / g) + (f - (e * (d / (c + (b - a)))));\n"
                   "let result : int = ((i + h) * (g - f) + (e / d)) - ((c + b) * (a - 1));\n"
                   "let compare = result == 13120;\n"
                   "if compare { let a = 10; let b = 2; return a * b == 20; } else { return false; }\n"
                   "}");

    vm_free(vm);
}

int main() {
    test_vm_execute();

    return 0;
}
