#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_int_modulo() {
    assert(test_run_int("func f(): i32 { let a: i32 = 7; let b: i32 = 3; return a % b; }\n"
                        "let r: i32 = f();\n") == 1);
}

static void test_modulo_divides_evenly() {
    assert(test_run_int("func f(): i32 { let a: i32 = 9; let b: i32 = 3; return a % b; }\n"
                        "let r: i32 = f();\n") == 0);
}

static void test_modulo_takes_the_sign_of_the_dividend() {
    assert(test_run_int("func f(): i32 { let a: i32 = -7; let b: i32 = 3; return a % b; }\n"
                        "let r: i32 = f();\n") == -1);

    assert(test_run_int("func f(): i32 { let a: i32 = 7; let b: i32 = -3; return a % b; }\n"
                        "let r: i32 = f();\n") == 1);
}

static void test_modulo_binds_as_tightly_as_multiplication() {
    assert(test_run_int("func f(): i32 { let a: i32 = 7; return 2 + a % 4; }\n"
                        "let r: i32 = f();\n") == 5);
}

static void test_modulo_is_left_associative() {
    assert(test_run_int("func f(): i32 { let a: i32 = 17; return a % 7 % 4; }\n"
                        "let r: i32 = f();\n") == 3);
}

static void test_modulo_by_zero_traps() {
    assert(test_run_status("func f(): i32 { let a: i32 = 1; let b: i32 = 0; return a % b; }\n"
                           "let r: i32 = f();\n") == VM_RUN_ERR_DIVIDE_BY_ZERO);
}

static void test_modulo_overflow_traps() {
    assert(test_run_status("func f(): i32 { let a: i32 = -2147483647 - 1; let b: i32 = -1; return a % b; }\n"
                           "let r: i32 = f();\n") == VM_RUN_ERR_DIVIDE_OVERFLOW);
}

static void test_moduli_near_the_trap_are_unaffected() {
    assert(test_run_int("func f(): i32 { let a: i32 = -2147483647; let b: i32 = -1; return a % b; }\n"
                        "let r: i32 = f();\n") == 0);

    assert(test_run_int("func f(): i32 { let a: i32 = -2147483647 - 1; let b: i32 = 1; return a % b; }\n"
                        "let r: i32 = f();\n") == 0);
}

static void test_modulo_is_typed_integral() {
    assert(test_compiles("func f(): i32 { let a: i32 = 7; let b: i32 = 3; return a % b; }\n"));

    assert(!test_compiles("func f(): f32 { let a: f32 = 7.0; let b: f32 = 3.0; return a % b; }\n"));
    assert(!test_compiles("func f(): bool { let a: bool = true; let b: bool = false; return a % b; }\n"));
}

int main() {
    test_int_modulo();
    test_modulo_divides_evenly();
    test_modulo_takes_the_sign_of_the_dividend();
    test_modulo_binds_as_tightly_as_multiplication();
    test_modulo_is_left_associative();
    test_modulo_by_zero_traps();
    test_modulo_overflow_traps();
    test_moduli_near_the_trap_are_unaffected();
    test_modulo_is_typed_integral();

    printf("modulo_test: all tests passed\n");
    return 0;
}
