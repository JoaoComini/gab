#include "support/run.h"

static void test_a_slice_is_unsized() {
    assert(!test_compiles("func f(): i32 { let xs: slice<i32>; return 0; }\n"));

    assert(!test_compiles("struct Holder { xs: slice<i32> }\n"
                          "func f(): i32 { return 0; }\n"));
}

static void test_an_array_lends_a_slice() {
    assert(test_run_int("func g(xs: &slice<i32>): i32 { return xs[1]; }\n"
                        "func f(): i32 { let xs: array<i32, 3> = [1, 20, 300]; return g(xs); }\n"
                        "let r: i32 = f();") == 20);
}

static void test_a_slice_carries_the_length_it_was_lent() {
    assert(test_run_int("func g(xs: &slice<i32>): i32 { return xs.len(); }\n"
                        "func f(): i32 { let xs: array<i32, 4>; return g(xs); }\n"
                        "let r: i32 = f();") == 4);
}

static void test_one_function_reads_arrays_of_every_length() {
    assert(test_run_int("func total(xs: &slice<i32>): i32 {\n"
                        "    let sum: i32 = 0;\n"
                        "    for let i: i32 = 0; i < xs.len(); i = i + 1 { sum = sum + xs[i]; }\n"
                        "    return sum;\n"
                        "}\n"
                        "func f(): i32 {\n"
                        "    let a: array<i32, 2> = [1, 2];\n"
                        "    let b: array<i32, 3> = [10, 20, 30];\n"
                        "    return total(a) + total(b);\n"
                        "}\n"
                        "let r: i32 = f();") == 63);
}

static void test_an_index_is_checked_against_the_length_the_slice_carries() {
    assert(test_run_int("func g(xs: &slice<i32>): i32 { return xs[3]; }\n"
                        "func f(): i32 { let xs: array<i32, 4> = [1, 2, 3, 40]; return g(xs); }\n"
                        "let r: i32 = f();") == 40);

    assert(test_run_status("func g(xs: &slice<i32>): i32 { return xs[3]; }\n"
                           "func f(): i32 { let xs: array<i32, 2>; return g(xs); }\n"
                           "let r: i32 = f();") != VM_RUN_OK);
}

static void test_a_slice_element_may_be_written_through() {
    assert(test_run_int("func set(xs: &slice<i32>): i32 { xs[0] = 9; return 0; }\n"
                        "func f(): i32 {\n"
                        "    let xs: array<i32, 2>;\n"
                        "    set(xs);\n"
                        "    return xs[0];\n"
                        "}\n"
                        "let r: i32 = f();") == 9);
}

static void test_an_element_type_must_match() {
    assert(!test_compiles("func g(xs: &slice<i32>): i32 { return 0; }\n"
                          "func f(): i32 { let xs: array<bool, 2>; return g(xs); }\n"));
}

static void test_a_slice_supplies_index() {
    assert(test_run_int("func first<C: Index<i32>>(c: &C): i32 { return *c.index(0); }\n"
                        "func g(xs: &slice<i32>): i32 { return first(xs); }\n"
                        "func f(): i32 { let xs: array<i32, 3> = [7, 20, 300]; return g(xs); }\n"
                        "let r: i32 = f();") == 7);
}

int main() {
    test_a_slice_is_unsized();
    test_an_array_lends_a_slice();
    test_a_slice_carries_the_length_it_was_lent();
    test_one_function_reads_arrays_of_every_length();
    test_an_index_is_checked_against_the_length_the_slice_carries();
    test_a_slice_element_may_be_written_through();
    test_an_element_type_must_match();
    test_a_slice_supplies_index();

    return 0;
}
