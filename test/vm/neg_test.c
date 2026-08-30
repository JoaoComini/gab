#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static void test_negates_a_literal() {
    assert(test_run_int("func f(): int { return -7; }\n"
                        "let r: int = f();\n") == -7);
}

static void test_negates_a_variable() {
    assert(test_run_int("func f(): int { let x: int = 7; return -x; }\n"
                        "let r: int = f();\n") == -7);
}

static void test_negates_a_float() {
    assert(test_run_float("func f(): float { let x: float = 2.5; return -x; }\n"
                          "let r: float = f();\n") == -2.5f);
}

static void test_negates_a_float_literal() {
    assert(test_run_float("func f(): float { return -2.5; }\n"
                          "let r: float = f();\n") == -2.5f);
}

static void test_prefix_and_binary_minus_coexist() {
    assert(test_run_int("func f(): int { let x: int = 10; return -x - -3; }\n"
                        "let r: int = f();\n") == -7);
}

static void test_binds_tighter_than_a_binary_operator() {
    assert(test_run_int("func f(): int { let x: int = 2; return -x - 3; }\n"
                        "let r: int = f();\n") == -5);
}

static void test_binds_looser_than_a_postfix() {
    assert(test_run_int("struct Point { x: int, y: int }\n"
                        "func f(): int { let v: Point; v.x = 4; return -v.x; }\n"
                        "let r: int = f();\n") == -4);
}

static void test_negates_a_call_result() {
    assert(test_run_int("func five(): int { return 5; }\n"
                        "func f(): int { return -five(); }\n"
                        "let r: int = f();\n") == -5);
}

static void test_negates_through_a_deref() {
    assert(test_run_int("func f(): int { let x: int = 9; let p: ref int = x; return -*p; }\n"
                        "let r: int = f();\n") == -9);
}

static void test_double_negation_cancels() {
    assert(test_run_int("func f(): int { let x: int = 6; return --x; }\n"
                        "let r: int = f();\n") == 6);
}

static void test_negating_int_min_wraps() {
    assert(test_run_int("func f(): int { let x: int = -2147483647 - 1; return -x; }\n"
                        "let r: int = f();\n") == INT32_MIN);

    assert(test_run_int("func f(): int { return -(-2147483647 - 1); }\n"
                        "let r: int = f();\n") == INT32_MIN);
}

static void test_negation_is_typed_numeric() {
    assert(test_compiles("func f(): int { let x: int = 1; return -x; }\n"));
    assert(test_compiles("func f(): float { let x: float = 1.0; return -x; }\n"));

    assert(!test_compiles("func f(): bool { let b: bool = true; return -b; }\n"));

    assert(!test_compiles("func f(): int { let x: int = 1; let p: ref int = x; return -p; }\n"));
}

static void test_negation_is_a_temporary() {
    assert(!test_compiles("func f(): int { let x: int = 1; -x = 2; return x; }\n"));
    assert(!test_compiles("func f(): int { let x: int = 1; let p: ref int = -x; return *p; }\n"));
}

int main() {
    test_negates_a_literal();
    test_negates_a_variable();
    test_negates_a_float();
    test_negates_a_float_literal();
    test_prefix_and_binary_minus_coexist();
    test_binds_tighter_than_a_binary_operator();
    test_binds_looser_than_a_postfix();
    test_negates_a_call_result();
    test_negates_through_a_deref();
    test_double_negation_cancels();
    test_negating_int_min_wraps();
    test_negation_is_typed_numeric();
    test_negation_is_a_temporary();

    printf("neg_test: all tests passed\n");
    return 0;
}
