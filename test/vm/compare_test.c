#include "slot.h"
#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static bool cmp_int(int a, const char *op, int b) {
    char source[256];

    snprintf(source, sizeof(source),
             "func f(): bool { let a: int = %d; let b: int = %d; return a %s b; }\n"
             "let r: bool = f();\n",
             a, b, op);

    return test_run_bool(source);
}

static bool cmp_float(double a, const char *op, double b) {
    char source[256];

    snprintf(source, sizeof(source),
             "func f(): bool { let a: float = %.1f; let b: float = %.1f; return a %s b; }\n"
             "let r: bool = f();\n",
             a, b, op);

    return test_run_bool(source);
}

static void test_int_less() {
    assert(cmp_int(3, "<", 5));
    assert(!cmp_int(5, "<", 3));
    assert(!cmp_int(4, "<", 4));
}

static void test_int_greater() {
    assert(cmp_int(5, ">", 3));
    assert(!cmp_int(3, ">", 5));
    assert(!cmp_int(4, ">", 4));
}

static void test_int_less_equal() {
    assert(cmp_int(3, "<=", 5));
    assert(!cmp_int(5, "<=", 3));
    assert(cmp_int(4, "<=", 4));
}

static void test_int_greater_equal() {
    assert(cmp_int(5, ">=", 3));
    assert(!cmp_int(3, ">=", 5));
    assert(cmp_int(4, ">=", 4));
}

static void test_int_equal() {
    assert(cmp_int(4, "==", 4));
    assert(!cmp_int(4, "==", 5));
}

static void test_int_not_equal() {
    assert(cmp_int(4, "!=", 5));
    assert(!cmp_int(4, "!=", 4));
}

static void test_float_less() {
    assert(cmp_float(3.0, "<", 5.0));
    assert(!cmp_float(5.0, "<", 3.0));
    assert(!cmp_float(4.0, "<", 4.0));
}

static void test_float_greater() {
    assert(cmp_float(5.0, ">", 3.0));
    assert(!cmp_float(3.0, ">", 5.0));
    assert(!cmp_float(4.0, ">", 4.0));
}

static void test_float_less_equal() {
    assert(cmp_float(3.0, "<=", 5.0));
    assert(!cmp_float(5.0, "<=", 3.0));
    assert(cmp_float(4.0, "<=", 4.0));
}

static void test_float_greater_equal() {
    assert(cmp_float(5.0, ">=", 3.0));
    assert(!cmp_float(3.0, ">=", 5.0));
    assert(cmp_float(4.0, ">=", 4.0));
}

static void test_float_equal() {
    assert(cmp_float(4.0, "==", 4.0));
    assert(!cmp_float(4.0, "==", 5.0));
}

static void test_float_not_equal() {
    assert(cmp_float(4.0, "!=", 5.0));
    assert(!cmp_float(4.0, "!=", 4.0));
}

static void test_immediate_operand_compares_the_same() {
    assert(test_run_bool("func f(): bool { let a: int = 5; return a >= 3; }\n"
                         "let r: bool = f();\n"));

    assert(!test_run_bool("func f(): bool { let a: int = 3; return a >= 5; }\n"
                          "let r: bool = f();\n"));

    assert(test_run_bool("func f(): bool { let a: int = 4; return a >= 4; }\n"
                         "let r: bool = f();\n"));
}

static void test_a_float_literal_compares_the_same() {
    assert(test_run_bool("func f(): bool { let a: float = 1.5; return a < 2.5; }\n"
                         "let r: bool = f();\n"));

    assert(!test_run_bool("func f(): bool { let a: float = 3.5; return a < 2.5; }\n"
                          "let r: bool = f();\n"));

    assert(test_run_bool("func f(): bool { let a: float = 1.5; return a > 0.5; }\n"
                         "let r: bool = f();\n"));

    assert(test_run_bool("func f(): bool { let a: float = 1.5; return a <= 1.5; }\n"
                         "let r: bool = f();\n"));

    assert(test_run_bool("func f(): bool { let a: float = 1.5; return a >= 1.5; }\n"
                         "let r: bool = f();\n"));

    assert(test_run_bool("func f(): bool { let a: float = 1.5; return a == 1.5; }\n"
                         "let r: bool = f();\n"));

    assert(test_run_bool("func f(): bool { let a: float = 1.5; return a != 2.5; }\n"
                         "let r: bool = f();\n"));
}

int main() {
    test_int_less();
    test_int_greater();
    test_int_less_equal();
    test_int_greater_equal();
    test_int_equal();
    test_int_not_equal();

    test_float_less();
    test_float_greater();
    test_float_less_equal();
    test_float_greater_equal();
    test_float_equal();
    test_float_not_equal();

    test_immediate_operand_compares_the_same();
    test_a_float_literal_compares_the_same();

    printf("compare_test: all tests passed\n");
    return 0;
}
