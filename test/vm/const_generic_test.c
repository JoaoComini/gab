#include "support/run.h"

static void test_a_declaration_is_generic_over_a_length() {
    assert(test_run_int("func first<T, N: i32>(xs: &array<T, N>): T { return xs[0]; }\n"
                        "func f(): i32 { let xs: array<i32, 3> = [7, 8, 9]; return first(xs); }\n"
                        "let r: i32 = f();") == 7);
}

static void test_a_length_reaches_the_body_as_a_count() {
    assert(test_run_int("func count<T, N: i32>(xs: &array<T, N>): i32 { return xs.len(); }\n"
                        "func f(): i32 { let xs: array<i32, 4>; return count(xs); }\n"
                        "let r: i32 = f();") == 4);
}

static void test_each_length_gets_its_own_instantiation() {
    assert(test_run_int("func count<T, N: i32>(xs: &array<T, N>): i32 { return xs.len(); }\n"
                        "func f(): i32 {\n"
                        "    let a: array<i32, 2>;\n"
                        "    let b: array<i32, 5>;\n"
                        "    return count(a) + count(b);\n"
                        "}\n"
                        "let r: i32 = f();") == 7);
}

static void test_a_length_is_inferred_from_the_argument() {
    assert(!test_compiles("func count<T, N: i32>(xs: &array<T, N>): i32 { return xs.len(); }\n"
                          "func f(): i32 { let xs: array<bool, 2>; return count(xs) + count(true); }\n"));
}

int main(void) {
    test_a_declaration_is_generic_over_a_length();
    test_a_length_reaches_the_body_as_a_count();
    test_each_length_gets_its_own_instantiation();
    test_a_length_is_inferred_from_the_argument();

    return 0;
}
