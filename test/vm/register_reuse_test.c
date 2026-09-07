#include "compile.h"
#include "support/run.h"
#include "vm/opcode.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int func_max_registers(VM *vm, size_t index) {
    size_t at = test_prelude_funcs() + index;

    assert(at < vm->program.prototypes.size);

    return vm->program.prototypes.data[at]->max_registers;
}

static int compile_max_registers(const char *source, size_t index) {
    VM *vm = vm_create();

    compile_and_run(vm, test_in_a_module(source));

    int result = func_max_registers(vm, index);

    vm_free(vm);

    return result;
}

static void test_frame_size_is_flat_in_statement_count() {
    int few = compile_max_registers("func f(n: int): int {\n"
                                    "let a = n + 1;\n"
                                    "let b = n + 2;\n"
                                    "return a + b;\n"
                                    "}\n",
                                    0);

    int many = compile_max_registers("func f(n: int): int {\n"
                                     "let a = n + 1;\n"
                                     "let b = n + 2;\n"
                                     "let c = n + 3;\n"
                                     "let d = n + 4;\n"
                                     "let e = n + 5;\n"
                                     "let g = n + 6;\n"
                                     "let h = n + 7;\n"
                                     "let i = n + 8;\n"
                                     "return a + b;\n"
                                     "}\n",
                                     0);

    /* Six more locals cost six more slots; the temporaries they are computed in are shared. */
    assert(many - few == 6);
}

static void test_block_locals_are_reclaimed() {
    const char *two_inner = "func f(n: int): int {\n"
                            "let a = n + 1;\n"
                            "if n > 0 {\n"
                            "let b = n + 2;\n"
                            "let c = n + 3;\n"
                            "}\n"
                            "let d = n + 4;\n"
                            "return a + d;\n"
                            "}\n";

    const char *four_inner = "func f(n: int): int {\n"
                             "let a = n + 1;\n"
                             "if n > 0 {\n"
                             "let b = n + 2;\n"
                             "let c = n + 3;\n"
                             "}\n"
                             "if n > 0 {\n"
                             "let e = n + 5;\n"
                             "let g = n + 6;\n"
                             "}\n"
                             "let d = n + 4;\n"
                             "return a + d;\n"
                             "}\n";

    assert(compile_max_registers(two_inner, 0) == compile_max_registers(four_inner, 0));
}

static void test_a_long_function_fits_in_the_frame() {
    assert(test_run_int("func f(n: int): int {\n"
                        "let s01 = n + 1; let s02 = n + 1; let s03 = n + 1; let s04 = n + 1;\n"
                        "let s05 = n + 1; let s06 = n + 1; let s07 = n + 1; let s08 = n + 1;\n"
                        "let s09 = n + 1; let s10 = n + 1; let s11 = n + 1; let s12 = n + 1;\n"
                        "let s13 = n + 1; let s14 = n + 1; let s15 = n + 1; let s16 = n + 1;\n"
                        "let s17 = n + 1; let s18 = n + 1; let s19 = n + 1; let s20 = n + 1;\n"
                        "let s21 = n + 1; let s22 = n + 1; let s23 = n + 1; let s24 = n + 1;\n"
                        "let s25 = n + 1; let s26 = n + 1; let s27 = n + 1; let s28 = n + 1;\n"
                        "let s29 = n + 1; let s30 = n + 1; let s31 = n + 1; let s32 = n + 1;\n"
                        "let s33 = n + 1; let s34 = n + 1; let s35 = n + 1; let s36 = n + 1;\n"
                        "let s37 = n + 1; let s38 = n + 1; let s39 = n + 1; let s40 = n + 1;\n"
                        "let s41 = n + 1; let s42 = n + 1; let s43 = n + 1;\n"
                        "return s43;\n"
                        "}\n"
                        "func main(): int { return f(41); }\n"
                        "let r: int = main();") == 42);
}

static void test_expression_statements_reuse_slots() {
    int one = compile_max_registers("func f(n: int): int { n + 1; return n; }\n", 0);
    int several = compile_max_registers("func f(n: int): int { n + 1; n + 2; n + 3; n + 4; return n; }\n", 0);

    assert(one == several);
}

static void test_call_result_survives_in_larger_expression() {
    assert(test_run_int("func add(a: int, b: int): int { return a + b; }\n"
                        "func main(): int { return add(1, 2) * 10 + add(3, 4); }\n"
                        "let r: int = main();") == 37);
}

static void test_nested_call_results_survive() {
    assert(test_run_int("func add(a: int, b: int): int { return a + b; }\n"
                        "func main(): int { return add(add(add(1, 2), add(3, 4)), add(5, 6)); }\n"
                        "let r: int = main();") == 21);
}

static int compile_sequential_lets(unsigned int count) {
    size_t capacity = 64 + (size_t)count * 24;
    char *source = malloc(capacity);
    size_t used = (size_t)snprintf(source, capacity, "func f(n: int): int {\n");

    for (unsigned int i = 0; i < count; i++) {
        used += (size_t)snprintf(source + used, capacity - used, "let s%u = n + 1;\n", i);
    }

    snprintf(source + used, capacity - used, "return n;\n}\nlet r: int = f(7);\n");

    VM *vm = vm_create();
    compile_and_run(vm, test_in_a_module(source));

    int32_t returned;
    memcpy(&returned, vm_slot_at(vm, test_result_slot(vm)), sizeof(returned));

    int result = returned == 7 ? func_max_registers(vm, 0) : -1;

    vm_free(vm);
    free(source);

    return result;
}

static void test_each_live_local_costs_one_slot() {
    int few = compile_sequential_lets(100);
    int more = compile_sequential_lets(150);

    assert(few > 0 && more > 0);

    assert(more - few == 50);
}

/* A frame is addressed by one byte, so a function needing more slots than that is rejected rather
 * than emitted with a slot the VM cannot name. */
static void test_a_function_too_large_for_the_frame_is_rejected() {
    unsigned int fits = 1;
    unsigned int fails = VM_MAX_FRAME_SLOTS;

    while (fails - fits > 1) {
        unsigned int mid = fits + (fails - fits) / 2;

        if (compile_sequential_lets(mid) > 0) {
            fits = mid;
        } else {
            fails = mid;
        }
    }

    assert(compile_sequential_lets(fits) <= VM_MAX_FRAME_SLOTS);
    assert(compile_sequential_lets(fails) == -1);
}

int main() {
    test_each_live_local_costs_one_slot();
    test_a_function_too_large_for_the_frame_is_rejected();
    test_frame_size_is_flat_in_statement_count();
    test_block_locals_are_reclaimed();
    test_a_long_function_fits_in_the_frame();
    test_expression_statements_reuse_slots();
    test_call_result_survives_in_larger_expression();
    test_nested_call_results_survive();

    printf("register_reuse_test: all tests passed\n");

    return 0;
}
