#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_int_to_float() {
    assert(test_run_float("func f(): f32 { let a: i32 = 3; return f32(a); }\n"
                          "let r: f32 = f();\n") == 3.0f);
}

static void test_float_to_int_truncates() {
    assert(test_run_int("func f(): i32 { let a: f32 = 2.7; return i32(a); }\n"
                        "let r: i32 = f();\n") == 2);
}

static void test_float_to_int_truncates_toward_zero() {
    assert(test_run_int("func f(): i32 { let a: f32 = -2.7; return i32(a); }\n"
                        "let r: i32 = f();\n") == -2);
}

static void test_cast_of_a_literal() {
    assert(test_run_int("func f(): i32 { return i32(2.7); }\n"
                        "let r: i32 = f();\n") == 2);

    assert(test_run_float("func f(): f32 { return f32(3); }\n"
                          "let r: f32 = f();\n") == 3.0f);
}

static void test_cast_of_an_expression() {
    assert(test_run_int("func f(): i32 { let a: f32 = 1.5; let b: f32 = 2.0; return i32(a + b); }\n"
                        "let r: i32 = f();\n") == 3);
}

static void test_cast_joins_the_two_numeric_types() {
    assert(test_run_float("func f(): f32 { let a: i32 = 1; let b: f32 = 2.5; return f32(a) + b; }\n"
                          "let r: f32 = f();\n") == 3.5f);

    assert(test_run_int("func f(): i32 { let a: i32 = 1; let b: f32 = 2.5; return a + i32(b); }\n"
                        "let r: i32 = f();\n") == 3);
}

static void test_cast_to_the_same_type() {
    assert(test_run_int("func f(): i32 { let a: i32 = 5; return i32(a); }\n"
                        "let r: i32 = f();\n") == 5);
}

static void test_float_to_int_clamps_out_of_range() {
    assert(test_run_int("func f(): i32 { let a: f32 = 2147483648.0; return i32(a); }\n"
                        "let r: i32 = f();\n") == 2147483647);

    assert(test_run_int("func f(): i32 { let a: f32 = 1000000000000.0; return i32(a); }\n"
                        "let r: i32 = f();\n") == 2147483647);

    assert(test_run_int("func f(): i32 { let a: f32 = -1000000000000.0; return i32(a); }\n"
                        "let r: i32 = f();\n") == (-2147483647 - 1));
}

static void test_infinity_clamps() {
    assert(test_run_int("func f(): i32 { let a: f32 = 1.0; let b: f32 = 0.0; return i32(a / b); }\n"
                        "let r: i32 = f();\n") == 2147483647);

    assert(test_run_int("func f(): i32 { let a: f32 = -1.0; let b: f32 = 0.0; return i32(a / b); }\n"
                        "let r: i32 = f();\n") == (-2147483647 - 1));
}

static void test_nan_converts_to_zero() {
    assert(test_run_int("func f(): i32 { let a: f32 = 0.0; let b: f32 = 0.0; return i32(a / b); }\n"
                        "let r: i32 = f();\n") == 0);
}

static void test_a_conversion_never_fails_the_run() {
    assert(test_run_status("func f(): i32 { let a: f32 = 1000000000000.0; return i32(a); }\n"
                           "let r: i32 = f();\n") == VM_RUN_OK);

    assert(test_run_status("func f(): i32 { let a: f32 = 0.0; let b: f32 = 0.0; return i32(a / b); }\n"
                           "let r: i32 = f();\n") == VM_RUN_OK);
}

static void test_casts_near_the_range_are_unaffected() {
    assert(test_run_int("func f(): i32 { let a: f32 = 1073741824.0; return i32(a); }\n"
                        "let r: i32 = f();\n") == 1073741824);

    assert(test_run_int("func f(): i32 { let a: f32 = -2147483648.0; return i32(a); }\n"
                        "let r: i32 = f();\n") == (-2147483647 - 1));

    assert(test_run_int("func f(): i32 { let a: f32 = -2147483520.0; return i32(a); }\n"
                        "let r: i32 = f();\n") == -2147483520);
}

static void test_only_numeric_types_convert() {
    assert(test_compiles("func f(): i32 { let a: f32 = 1.0; return i32(a); }\n"));
    assert(test_compiles("func f(): f32 { let a: i32 = 1; return f32(a); }\n"));

    assert(!test_compiles("func f(): i32 { let a: bool = true; return i32(a); }\n"));
    assert(!test_compiles("func f(): bool { let a: i32 = 1; return bool(a); }\n"));
}

static void test_a_cast_takes_one_operand() {
    assert(!test_compiles("func f(): i32 { return i32(); }\n"));
    assert(!test_compiles("func f(): i32 { let a: f32 = 1.0; return i32(a, a); }\n"));
}

static void test_a_struct_name_is_not_a_cast() {
    assert(!test_compiles("struct Point { x: i32 }\n"
                          "func f(): i32 { let a: i32 = 1; return Point(a).x; }\n"));
}

static void test_a_cast_is_a_temporary() {
    assert(!test_compiles("func f(): i32 { let a: f32 = 1.0; i32(a) = 2; return 1; }\n"));
    assert(!test_compiles("func f(): i32 { let a: f32 = 1.0; let p: &i32 = i32(a); return *p; }\n"));
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
