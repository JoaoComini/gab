#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_int_divide_by_zero_traps() {
    assert(test_run_status("func f(): i32 { let a: i32 = 1; let b: i32 = 0; return a / b; }\n"
                           "let r: i32 = f();\n") == VM_RUN_ERR_DIVIDE_BY_ZERO);
}

static void test_int_divide_overflow_traps() {
    assert(test_run_status("func f(): i32 { let a: i32 = -2147483647 - 1; let b: i32 = -1; return a / b; }\n"
                           "let r: i32 = f();\n") == VM_RUN_ERR_DIVIDE_OVERFLOW);
}

static void test_float_divide_by_zero_is_not_a_trap() {
    assert(test_run_status("func f(): f32 { let a: f32 = 1.0; let b: f32 = 0.0; return a / b; }\n"
                           "let r: f32 = f();\n") == VM_RUN_OK);
}

static void test_a_folded_constant_computes_what_the_vm_would() {
    assert(test_run_int("let r: i32 = 7 % 2;\n") == 1);
    assert(test_run_float("let r: f32 = 0.0 - 9.8;\n") == -9.8f);
}

static void test_a_folded_division_by_zero_still_traps() {
    assert(test_run_status("func f(): i32 { return 1 / 0; }\nlet r: i32 = f();\n") ==
           VM_RUN_ERR_DIVIDE_BY_ZERO);
    assert(test_run_status("func f(): i32 { return 1 % 0; }\nlet r: i32 = f();\n") ==
           VM_RUN_ERR_DIVIDE_BY_ZERO);
}

int main() {

    test_int_divide_by_zero_traps();
    test_int_divide_overflow_traps();
    test_float_divide_by_zero_is_not_a_trap();

    test_a_folded_constant_computes_what_the_vm_would();
    test_a_folded_division_by_zero_still_traps();

    printf("arithmetic_test: all tests passed\n");
    return 0;
}
