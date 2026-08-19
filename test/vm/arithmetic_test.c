// What the arithmetic opcodes actually compute. The codegen tests check which
// opcode each operator emits and the comparison tests cover the CMP family;
// this covers the other half, which nothing asserted on -- the same gap that
// let OP_CMP_GE dispatch to the wrong helper for both int and float.
//
// Operands are locals rather than literals wherever the value must reach a
// register, since a literal right operand takes the immediate encoding
// instead. Both paths are walked.
#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_int_add() {
    assert(test_run_int("func f(): int { let a: int = 2; let b: int = 3; return a + b; }\n"
                        "let r: int = f();\n") == 5);

    // Order matters for the non-commutative ops below; addition is checked
    // with a negative operand instead, which is where a sign slip would show.
    assert(test_run_int("func f(): int { let a: int = 2; let b: int = -5; return a + b; }\n"
                        "let r: int = f();\n") == -3);
}

// Subtraction and division are the two where swapped operands still produce a
// plausible-looking number, so both are checked with distinct operands.
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

    // Truncation, not rounding: 7/2 is 3.
    assert(test_run_int("func f(): int { let a: int = 7; let b: int = 2; return a / b; }\n"
                        "let r: int = f();\n") == 3);

    // Operand order, which a swap would turn into 0.
    assert(test_run_int("func f(): int { let a: int = 20; let b: int = 4; return b / a; }\n"
                        "let r: int = f();\n") == 0);
}

// The immediate encoding is a second path through every integer operator: the
// right operand rides in the instruction rather than a register.
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

// Float division does not truncate, which is the visible difference from the
// integer opcode and would be hidden by operands that divide evenly.
static void test_float_divide() {
    assert(test_run_float("func f(): float { let a: float = 7.0; let b: float = 2.0; return a / b; }\n"
                          "let r: float = f();\n") == 3.5f);

    assert(test_run_float("func f(): float { let a: float = 2.0; let b: float = 8.0; return a / b; }\n"
                          "let r: float = f();\n") == 0.25f);
}

// Precedence and associativity, which decide how the opcodes get ordered rather
// than what any one of them does. '2 + 3 * 4' is 14, not 20; '10 - 3 - 2' is 5,
// not 9.
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

// Integer division by zero would be undefined behaviour and did in fact take
// the host process down with SIGFPE. An embedded VM must not let a script kill
// its host, so it fails the run instead -- the same way a dangling weak deref
// already did.
static void test_int_divide_by_zero_traps() {
    assert(test_run_status("func f(): int { let a: int = 1; let b: int = 0; return a / b; }\n"
                           "let r: int = f();\n") == VM_RUN_ERR_DIVIDE_BY_ZERO);
}

// The other undefined case: INT32_MIN / -1 has no representable quotient, and
// traps on the same hardware instruction as division by zero.
static void test_int_divide_overflow_traps() {
    assert(test_run_status("func f(): int { let a: int = -2147483647 - 1; let b: int = -1; return a / b; }\n"
                           "let r: int = f();\n") == VM_RUN_ERR_DIVIDE_OVERFLOW);
}

// Only the divisor being -1 is a problem, and only for that one dividend:
// neighbouring values must still divide normally rather than being swept up by
// an over-broad guard.
static void test_neighbouring_divisions_still_work() {
    assert(test_run_int("func f(): int { let a: int = -2147483647; let b: int = -1; return a / b; }\n"
                        "let r: int = f();\n") == 2147483647);

    assert(test_run_int("func f(): int { let a: int = -2147483647 - 1; let b: int = 1; return a / b; }\n"
                        "let r: int = f();\n") == (-2147483647 - 1));

    assert(test_run_int("func f(): int { let a: int = -10; let b: int = -1; return a / b; }\n"
                        "let r: int = f();\n") == 10);
}

// Float division by zero is not a trap: IEEE gives an infinity, which is an
// ordinary value the VM can carry. Checked so the integer guard is not later
// copied onto the float opcode by symmetry.
static void test_float_divide_by_zero_is_not_a_trap() {
    assert(test_run_status("func f(): float { let a: float = 1.0; let b: float = 0.0; return a / b; }\n"
                           "let r: float = f();\n") == VM_RUN_OK);
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
    test_neighbouring_divisions_still_work();
    test_float_divide_by_zero_is_not_a_trap();

    printf("arithmetic_test: all tests passed\n");
    return 0;
}
