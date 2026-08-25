// What the comparison opcodes compute, as distinct from which opcode an
// operator emits: the two are separate claims, and only this one is what a
// program observes.
//
// Every operator is checked below its boundary, above it, and on equality. An
// inverted comparison still agrees with the correct one on one of those three,
// so fewer points would let a swapped operator pass.
#include "slot.h"
#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

// Builds 'a <op> b' over two int locals. They are locals rather than literals
// so the operand reaches a register: a literal right operand takes the
// immediate path instead, which is a different encoding.
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

// A literal right operand rides in the instruction instead of a register, so
// the immediate path is a second encoding of every integer comparison and is
// worth walking once.
static void test_immediate_operand_compares_the_same() {
    assert(test_run_bool("func f(): bool { let a: int = 5; return a >= 3; }\n"
                         "let r: bool = f();\n"));

    assert(!test_run_bool("func f(): bool { let a: int = 3; return a >= 5; }\n"
                          "let r: bool = f();\n"));

    assert(test_run_bool("func f(): bool { let a: int = 4; return a >= 4; }\n"
                         "let r: bool = f();\n"));
}

// A float literal on the right of an arithmetic operator rides in the constant
// pool, but the comparisons have no such opcode. Comparing against one is the
// case that has to fall back to loading it into a register.
static void test_a_float_literal_compares_the_same() {
    assert(test_run_bool("func f(): bool { let a: float = 1.5; return a < 2.5; }\n"
                         "let r: bool = f();\n"));

    assert(!test_run_bool("func f(): bool { let a: float = 3.5; return a < 2.5; }\n"
                          "let r: bool = f();\n"));

    // Each comparison separately: they share the fallback, and one opcode
    // reaching for the constant form is enough to have no instruction.
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
