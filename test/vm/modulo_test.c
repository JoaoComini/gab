// Remainder: 'a % b'. It is the one arithmetic operator that takes ints and
// only ints, and it shares division's two undefined operand pairs, so those
// are what the rules below pin down.
#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_int_modulo() {
    assert(test_run_int("func f(): int { let a: int = 7; let b: int = 3; return a % b; }\n"
                        "let r: int = f();\n") == 1);
}

// A remainder that comes out zero is the case an implementation returning the
// dividend unchanged would still pass, so it is checked apart from the above.
static void test_modulo_divides_evenly() {
    assert(test_run_int("func f(): int { let a: int = 9; let b: int = 3; return a % b; }\n"
                        "let r: int = f();\n") == 0);
}

// The remainder takes the sign of the dividend, not the divisor: this is C's
// truncated division rather than a floored one, which would give 1 here.
static void test_modulo_takes_the_sign_of_the_dividend() {
    assert(test_run_int("func f(): int { let a: int = -7; let b: int = 3; return a % b; }\n"
                        "let r: int = f();\n") == -1);

    assert(test_run_int("func f(): int { let a: int = 7; let b: int = -3; return a % b; }\n"
                        "let r: int = f();\n") == 1);
}

// '%' binds as tightly as '*' and '/', so this is 2 + (7 % 4) rather than
// (2 + 7) % 4. Those disagree: 5 against 1.
static void test_modulo_binds_as_tightly_as_multiplication() {
    assert(test_run_int("func f(): int { let a: int = 7; return 2 + a % 4; }\n"
                        "let r: int = f();\n") == 5);
}

// Left-associative, like the other operators at its precedence: (17 % 7) % 3
// is 3 % 3, which is 0, while 17 % (7 % 3) would be 17 % 1, also 0 -- so the
// operands are chosen to disagree.
static void test_modulo_is_left_associative() {
    assert(test_run_int("func f(): int { let a: int = 17; return a % 7 % 4; }\n"
                        "let r: int = f();\n") == 3);
}

// Modulo by zero is undefined in C and traps on the same hardware instruction
// as division by zero, so it fails the run rather than killing the host.
static void test_modulo_by_zero_traps() {
    assert(test_run_status("func f(): int { let a: int = 1; let b: int = 0; return a % b; }\n"
                           "let r: int = f();\n") == VM_RUN_ERR_DIVIDE_BY_ZERO);
}

// INT32_MIN % -1 is mathematically 0, but it is computed by the instruction
// whose quotient overflows, so it is undefined for the same reason and guarded
// alongside it.
static void test_modulo_overflow_traps() {
    assert(test_run_status("func f(): int { let a: int = -2147483647 - 1; let b: int = -1; return a % b; }\n"
                           "let r: int = f();\n") == VM_RUN_ERR_DIVIDE_OVERFLOW);
}

// The trap is narrow: only INT32_MIN paired with -1. Neighbouring values on
// both operands take a remainder normally.
static void test_moduli_near_the_trap_are_unaffected() {
    assert(test_run_int("func f(): int { let a: int = -2147483647; let b: int = -1; return a % b; }\n"
                        "let r: int = f();\n") == 0);

    assert(test_run_int("func f(): int { let a: int = -2147483647 - 1; let b: int = 1; return a % b; }\n"
                        "let r: int = f();\n") == 0);
}

// '%' is the one arithmetic operator that is not numeric but integral: a
// float remainder would need a libc call rather than an instruction.
static void test_modulo_is_typed_integral() {
    assert(test_compiles("func f(): int { let a: int = 7; let b: int = 3; return a % b; }\n"));

    assert(!test_compiles("func f(): float { let a: float = 7.0; let b: float = 3.0; return a % b; }\n"));
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
