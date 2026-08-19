// What the comparison opcodes actually compute. The codegen tests already
// check which opcode each operator emits, and the lexer tests check that the
// tokens scan; neither notices if an opcode's VM case runs the wrong helper.
// That gap let '>=' dispatch to the '<=' helper on both int and float.
//
// Every operator is checked on both sides of its boundary and on equality,
// because an inverted comparison agrees with the correct one on one of those
// three and only disagrees on the others.
#include "support/run.h"
#include "value.h"
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

// The regression: this dispatched to the '<=' helper, so it answered the
// opposite of the truth on both of the first two cases.
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

// The float half of the same regression.
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

    printf("compare_test: all tests passed\n");
    return 0;
}
