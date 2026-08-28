// Conversions between int and float, spelled as a call on the type name:
// 'int(f)' and 'float(i)'. These are the only conversions in the language --
// nothing happens implicitly -- so the rules here are what each one does to a
// value and which of them are refused.
#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_int_to_float() {
    assert(test_run_float("func f(): float { let a: int = 3; return float(a); }\n"
                          "let r: float = f();\n") == 3.0f);
}

static void test_float_to_int_truncates() {
    assert(test_run_int("func f(): int { let a: float = 2.7; return int(a); }\n"
                        "let r: int = f();\n") == 2);
}

// Truncation is toward zero, not a floor: a floor would give -3 here.
static void test_float_to_int_truncates_toward_zero() {
    assert(test_run_int("func f(): int { let a: float = -2.7; return int(a); }\n"
                        "let r: int = f();\n") == -2);
}

static void test_cast_of_a_literal() {
    assert(test_run_int("func f(): int { return int(2.7); }\n"
                        "let r: int = f();\n") == 2);

    assert(test_run_float("func f(): float { return float(3); }\n"
                          "let r: float = f();\n") == 3.0f);
}

// The operand is a whole expression, not just the token after '('.
static void test_cast_of_an_expression() {
    assert(test_run_int("func f(): int { let a: float = 1.5; let b: float = 2.0; return int(a + b); }\n"
                        "let r: int = f();\n") == 3);
}

// A cast is what lets the two types meet, since nothing converts implicitly.
static void test_cast_joins_the_two_numeric_types() {
    assert(test_run_float("func f(): float { let a: int = 1; let b: float = 2.5; return float(a) + b; }\n"
                          "let r: float = f();\n") == 3.5f);

    assert(test_run_int("func f(): int { let a: int = 1; let b: float = 2.5; return a + int(b); }\n"
                        "let r: int = f();\n") == 3);
}

// Casting to the type a value already has is allowed and does nothing.
static void test_cast_to_the_same_type() {
    assert(test_run_int("func f(): int { let a: int = 5; return int(a); }\n"
                        "let r: int = f();\n") == 5);
}

// A float too large for an int clamps to the nearest end of the range rather
// than failing the run or wrapping into it. A wrap would send 2^31 to the
// bottom of the range; clamping pins it at the top.
static void test_float_to_int_clamps_out_of_range() {
    assert(test_run_int("func f(): int { let a: float = 2147483648.0; return int(a); }\n"
                        "let r: int = f();\n") == 2147483647);

    assert(test_run_int("func f(): int { let a: float = 1000000000000.0; return int(a); }\n"
                        "let r: int = f();\n") == 2147483647);

    assert(test_run_int("func f(): int { let a: float = -1000000000000.0; return int(a); }\n"
                        "let r: int = f();\n") == (-2147483647 - 1));
}

// An infinity clamps like any other out-of-range value: it is beyond the limit
// in a known direction, so the limit is the nearest answer.
static void test_infinity_clamps() {
    assert(test_run_int("func f(): int { let a: float = 1.0; let b: float = 0.0; return int(a / b); }\n"
                        "let r: int = f();\n") == 2147483647);

    assert(test_run_int("func f(): int { let a: float = -1.0; let b: float = 0.0; return int(a / b); }\n"
                        "let r: int = f();\n") == (-2147483647 - 1));
}

// NaN is outside the range in no direction at all, so neither limit is nearer
// and it converts to zero. Distinguished from the infinities above, which do
// have a direction.
static void test_nan_converts_to_zero() {
    assert(test_run_int("func f(): int { let a: float = 0.0; let b: float = 0.0; return int(a / b); }\n"
                        "let r: int = f();\n") == 0);
}

// Nothing about a conversion can fail the run: every float has an answer here.
static void test_a_conversion_never_fails_the_run() {
    assert(test_run_status("func f(): int { let a: float = 1000000000000.0; return int(a); }\n"
                           "let r: int = f();\n") == VM_RUN_OK);

    assert(test_run_status("func f(): int { let a: float = 0.0; let b: float = 0.0; return int(a / b); }\n"
                           "let r: int = f();\n") == VM_RUN_OK);
}

// Values at the edges of the range convert unchanged, which is what an
// over-broad clamp would break. The powers of two are held exactly by a 32-bit
// float, so the round trip is not measuring rounding.
static void test_casts_near_the_range_are_unaffected() {
    assert(test_run_int("func f(): int { let a: float = 1073741824.0; return int(a); }\n"
                        "let r: int = f();\n") == 1073741824);

    // -2^31 is INT32_MIN itself: the most negative int, and the last value
    // below zero that still fits.
    assert(test_run_int("func f(): int { let a: float = -2147483648.0; return int(a); }\n"
                        "let r: int = f();\n") == (-2147483647 - 1));

    // The float just inside that bound truncates rather than clamping, which a
    // bound one representable step too tight would break. Floats just below
    // 2^31 are spaced 128 apart, so this is the immediate neighbour.
    assert(test_run_int("func f(): int { let a: float = -2147483520.0; return int(a); }\n"
                        "let r: int = f();\n") == -2147483520);
}

// Only the two numeric types convert. Bool is left out deliberately: 'bool(1)'
// would be int-as-truth-value, which nothing else in the language allows.
static void test_only_numeric_types_convert() {
    assert(test_compiles("func f(): int { let a: float = 1.0; return int(a); }\n"));
    assert(test_compiles("func f(): float { let a: int = 1; return float(a); }\n"));

    assert(!test_compiles("func f(): int { let a: bool = true; return int(a); }\n"));
    assert(!test_compiles("func f(): bool { let a: int = 1; return bool(a); }\n"));
}

// A conversion takes exactly one operand, so neither zero nor two is a cast.
static void test_a_cast_takes_one_operand() {
    assert(!test_compiles("func f(): int { return int(); }\n"));
    assert(!test_compiles("func f(): int { let a: float = 1.0; return int(a, a); }\n"));
}

// A struct type is not callable: only the numeric builtins convert.
static void test_a_struct_name_is_not_a_cast() {
    assert(!test_compiles("struct Point { x: int }\n"
                          "func f(): int { let a: int = 1; return Point(a).x; }\n"));
}

// The result is a fresh value, so it has no address and no home to assign to.
static void test_a_cast_is_a_temporary() {
    assert(!test_compiles("func f(): int { let a: float = 1.0; int(a) = 2; return 1; }\n"));
    assert(!test_compiles("func f(): int { let a: float = 1.0; let p: ref int = int(a); return *p; }\n"));
}

int main() {
    test_int_to_float();
    test_float_to_int_truncates();
    test_float_to_int_truncates_toward_zero();
    test_cast_of_a_literal();
    test_cast_of_an_expression();
    test_cast_joins_the_two_numeric_types();
    test_cast_to_the_same_type();
    test_float_to_int_clamps_out_of_range();
    test_infinity_clamps();
    test_nan_converts_to_zero();
    test_a_conversion_never_fails_the_run();
    test_casts_near_the_range_are_unaffected();
    test_only_numeric_types_convert();
    test_a_cast_takes_one_operand();
    test_a_struct_name_is_not_a_cast();
    test_a_cast_is_a_temporary();

    printf("cast_test: all tests passed\n");
    return 0;
}
