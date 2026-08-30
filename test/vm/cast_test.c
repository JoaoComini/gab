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

static void test_cast_of_an_expression() {
    assert(test_run_int("func f(): int { let a: float = 1.5; let b: float = 2.0; return int(a + b); }\n"
                        "let r: int = f();\n") == 3);
}

static void test_cast_joins_the_two_numeric_types() {
    assert(test_run_float("func f(): float { let a: int = 1; let b: float = 2.5; return float(a) + b; }\n"
                          "let r: float = f();\n") == 3.5f);

    assert(test_run_int("func f(): int { let a: int = 1; let b: float = 2.5; return a + int(b); }\n"
                        "let r: int = f();\n") == 3);
}

static void test_cast_to_the_same_type() {
    assert(test_run_int("func f(): int { let a: int = 5; return int(a); }\n"
                        "let r: int = f();\n") == 5);
}

static void test_float_to_int_clamps_out_of_range() {
    assert(test_run_int("func f(): int { let a: float = 2147483648.0; return int(a); }\n"
                        "let r: int = f();\n") == 2147483647);

    assert(test_run_int("func f(): int { let a: float = 1000000000000.0; return int(a); }\n"
                        "let r: int = f();\n") == 2147483647);

    assert(test_run_int("func f(): int { let a: float = -1000000000000.0; return int(a); }\n"
                        "let r: int = f();\n") == (-2147483647 - 1));
}

static void test_infinity_clamps() {
    assert(test_run_int("func f(): int { let a: float = 1.0; let b: float = 0.0; return int(a / b); }\n"
                        "let r: int = f();\n") == 2147483647);

    assert(test_run_int("func f(): int { let a: float = -1.0; let b: float = 0.0; return int(a / b); }\n"
                        "let r: int = f();\n") == (-2147483647 - 1));
}

static void test_nan_converts_to_zero() {
    assert(test_run_int("func f(): int { let a: float = 0.0; let b: float = 0.0; return int(a / b); }\n"
                        "let r: int = f();\n") == 0);
}

static void test_a_conversion_never_fails_the_run() {
    assert(test_run_status("func f(): int { let a: float = 1000000000000.0; return int(a); }\n"
                           "let r: int = f();\n") == VM_RUN_OK);

    assert(test_run_status("func f(): int { let a: float = 0.0; let b: float = 0.0; return int(a / b); }\n"
                           "let r: int = f();\n") == VM_RUN_OK);
}

static void test_casts_near_the_range_are_unaffected() {
    assert(test_run_int("func f(): int { let a: float = 1073741824.0; return int(a); }\n"
                        "let r: int = f();\n") == 1073741824);

    assert(test_run_int("func f(): int { let a: float = -2147483648.0; return int(a); }\n"
                        "let r: int = f();\n") == (-2147483647 - 1));

    assert(test_run_int("func f(): int { let a: float = -2147483520.0; return int(a); }\n"
                        "let r: int = f();\n") == -2147483520);
}

static void test_only_numeric_types_convert() {
    assert(test_compiles("func f(): int { let a: float = 1.0; return int(a); }\n"));
    assert(test_compiles("func f(): float { let a: int = 1; return float(a); }\n"));

    assert(!test_compiles("func f(): int { let a: bool = true; return int(a); }\n"));
    assert(!test_compiles("func f(): bool { let a: int = 1; return bool(a); }\n"));
}

static void test_a_cast_takes_one_operand() {
    assert(!test_compiles("func f(): int { return int(); }\n"));
    assert(!test_compiles("func f(): int { let a: float = 1.0; return int(a, a); }\n"));
}

static void test_a_struct_name_is_not_a_cast() {
    assert(!test_compiles("struct Point { x: int }\n"
                          "func f(): int { let a: int = 1; return Point(a).x; }\n"));
}

static void test_a_cast_is_a_temporary() {
    assert(!test_compiles("func f(): int { let a: float = 1.0; int(a) = 2; return 1; }\n"));
    assert(!test_compiles("func f(): int { let a: float = 1.0; let p: &int = int(a); return *p; }\n"));
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
