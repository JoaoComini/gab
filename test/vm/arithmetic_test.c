#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_int_add() {
    assert(test_run_int("func f(): int { let a: int = 2; let b: int = 3; return a + b; }\n"
                        "let r: int = f();\n") == 5);

    assert(test_run_int("func f(): int { let a: int = 2; let b: int = -5; return a + b; }\n"
                        "let r: int = f();\n") == -3);
}

static void test_int_subtract() {
    assert(test_run_int("func f(): int { let a: int = 10; let b: int = 3; return a - b; }\n"
                        "let r: int = f();\n") == 7);

    assert(test_run_int("func f(): int { let a: int = 3; let b: int = 10; return a - b; }\n"
                        "let r: int = f();\n") == -7);
}

static void test_int_multiply() {
    assert(test_run_int("func f(): int { let a: int = 6; let b: int = 7; return a * b; }\n"
                        "let r: int = f();\n") == 42);

    assert(test_run_int("func f(): int { let a: int = 6; let b: int = -7; return a * b; }\n"
                        "let r: int = f();\n") == -42);
}

static void test_int_divide() {
    assert(test_run_int("func f(): int { let a: int = 20; let b: int = 4; return a / b; }\n"
                        "let r: int = f();\n") == 5);

    assert(test_run_int("func f(): int { let a: int = 7; let b: int = 2; return a / b; }\n"
                        "let r: int = f();\n") == 3);

    assert(test_run_int("func f(): int { let a: int = 20; let b: int = 4; return b / a; }\n"
                        "let r: int = f();\n") == 0);
}

static void test_int_immediate_operand() {
    assert(test_run_int("func f(): int { let a: int = 10; return a + 3; }\n"
                        "let r: int = f();\n") == 13);

    assert(test_run_int("func f(): int { let a: int = 10; return a - 3; }\n"
                        "let r: int = f();\n") == 7);

    assert(test_run_int("func f(): int { let a: int = 10; return a * 3; }\n"
                        "let r: int = f();\n") == 30);

    assert(test_run_int("func f(): int { let a: int = 10; return a / 3; }\n"
                        "let r: int = f();\n") == 3);
}

static void test_float_add() {
    assert(test_run_float("func f(): float { let a: float = 2.5; let b: float = 0.25; return a + b; }\n"
                          "let r: float = f();\n") == 2.75f);
}

static void test_float_subtract() {
    assert(test_run_float("func f(): float { let a: float = 2.5; let b: float = 0.25; return a - b; }\n"
                          "let r: float = f();\n") == 2.25f);

    assert(test_run_float("func f(): float { let a: float = 0.25; let b: float = 2.5; return a - b; }\n"
                          "let r: float = f();\n") == -2.25f);
}

static void test_float_multiply() {
    assert(test_run_float("func f(): float { let a: float = 2.5; let b: float = 4.0; return a * b; }\n"
                          "let r: float = f();\n") == 10.0f);
}

static void test_float_divide() {
    assert(test_run_float("func f(): float { let a: float = 7.0; let b: float = 2.0; return a / b; }\n"
                          "let r: float = f();\n") == 3.5f);

    assert(test_run_float("func f(): float { let a: float = 2.0; let b: float = 8.0; return a / b; }\n"
                          "let r: float = f();\n") == 0.25f);
}

static void test_precedence_and_associativity() {
    assert(test_run_int("func f(): int { return 2 + 3 * 4; }\n"
                        "let r: int = f();\n") == 14);

    assert(test_run_int("func f(): int { return (2 + 3) * 4; }\n"
                        "let r: int = f();\n") == 20);

    assert(test_run_int("func f(): int { let a: int = 10; return a - 3 - 2; }\n"
                        "let r: int = f();\n") == 5);

    assert(test_run_int("func f(): int { let a: int = 100; return a / 5 / 2; }\n"
                        "let r: int = f();\n") == 10);
}

static void test_int_divide_by_zero_traps() {
    assert(test_run_status("func f(): int { let a: int = 1; let b: int = 0; return a / b; }\n"
                           "let r: int = f();\n") == VM_RUN_ERR_DIVIDE_BY_ZERO);
}

static void test_int_divide_overflow_traps() {
    assert(test_run_status("func f(): int { let a: int = -2147483647 - 1; let b: int = -1; return a / b; }\n"
                           "let r: int = f();\n") == VM_RUN_ERR_DIVIDE_OVERFLOW);
}

static void test_divisions_near_the_trap_are_unaffected() {
    assert(test_run_int("func f(): int { let a: int = -2147483647; let b: int = -1; return a / b; }\n"
                        "let r: int = f();\n") == 2147483647);

    assert(test_run_int("func f(): int { let a: int = -2147483647 - 1; let b: int = 1; return a / b; }\n"
                        "let r: int = f();\n") == (-2147483647 - 1));

    assert(test_run_int("func f(): int { let a: int = -10; let b: int = -1; return a / b; }\n"
                        "let r: int = f();\n") == 10);
}

static void test_float_divide_by_zero_is_not_a_trap() {
    assert(test_run_status("func f(): float { let a: float = 1.0; let b: float = 0.0; return a / b; }\n"
                           "let r: float = f();\n") == VM_RUN_OK);
}

static void test_a_folded_constant_computes_what_the_vm_would() {
    assert(test_run_int("let r: int = 2 + 3 * 4;\n") == 14);
    assert(test_run_int("let r: int = (7 - 2) * 3;\n") == 15);
    assert(test_run_int("let r: int = 7 / 2;\n") == 3);
    assert(test_run_int("let r: int = 7 % 2;\n") == 1);
    assert(test_run_int("let r: int = -7 / 2;\n") == -3);
    assert(test_run_int("let r: int = -7 % 2;\n") == -1);

    assert(test_run_int("let r: int = 2147483647 + 1;\n") == -2147483647 - 1);

    assert(test_run_float("let r: float = 1.5 * 2.0;\n") == 3.0f);
    assert(test_run_float("let r: float = 0.0 - 9.8;\n") == -9.8f);
}

static void test_a_folded_division_by_zero_still_traps() {
    assert(test_run_status("func f(): int { return 1 / 0; }\nlet r: int = f();\n") ==
           VM_RUN_ERR_DIVIDE_BY_ZERO);
    assert(test_run_status("func f(): int { return 1 % 0; }\nlet r: int = f();\n") ==
           VM_RUN_ERR_DIVIDE_BY_ZERO);
}

int main() {
    test_int_add();
    test_int_subtract();
    test_int_multiply();
    test_int_divide();
    test_int_immediate_operand();

    test_float_add();
    test_float_subtract();
    test_float_multiply();
    test_float_divide();

    test_precedence_and_associativity();

    test_int_divide_by_zero_traps();
    test_int_divide_overflow_traps();
    test_divisions_near_the_trap_are_unaffected();
    test_float_divide_by_zero_is_not_a_trap();

    test_a_folded_constant_computes_what_the_vm_would();
    test_a_folded_division_by_zero_still_traps();

    printf("arithmetic_test: all tests passed\n");
    return 0;
}
