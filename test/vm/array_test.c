#include "support/run.h"

static void test_a_fixed_array_holds_its_length_in_its_type() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 3>;\n"
                        "    xs[1] = 7;\n"
                        "    return xs[1];\n"
                        "}\n"
                        "let r: i32 = f();") == 7);
}

static void test_elements_are_distinct() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 3>;\n"
                        "    xs[0] = 1;\n"
                        "    xs[1] = 20;\n"
                        "    xs[2] = 300;\n"
                        "    return xs[0] + xs[1] + xs[2];\n"
                        "}\n"
                        "let r: i32 = f();") == 321);
}

static void test_an_array_starts_zeroed() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 3>;\n"
                        "    return xs[0] + xs[1] + xs[2];\n"
                        "}\n"
                        "let r: i32 = f();") == 0);
}

static void test_the_elements_may_be_written_out() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 3> = [1, 20, 300];\n"
                        "    return xs[0] + xs[1] + xs[2];\n"
                        "}\n"
                        "let r: i32 = f();") == 321);

    assert(!test_compiles("func f(): i32 {\n"
                          "    let xs: array<i32, 3> = [1, 2];\n"
                          "    return 0;\n"
                          "}\n"));

    assert(!test_compiles("func f(): i32 {\n"
                          "    let xs: array<i32, 2> = [1, true];\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_elements_may_be_written_wherever_the_type_is_known() {
    assert(test_run_int("func g(xs: array<i32, 2>): i32 { return xs[1]; }\n"
                        "func f(): i32 { return g([7, 8]); }\n"
                        "let r: i32 = f();") == 8);

    assert(test_run_int("func g(): array<i32, 2> { return [3, 4]; }\n"
                        "func f(): i32 { let xs: array<i32, 2> = g(); return xs[0]; }\n"
                        "let r: i32 = f();") == 3);

    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 2>;\n"
                        "    xs = [5, 6];\n"
                        "    return xs[1];\n"
                        "}\n"
                        "let r: i32 = f();") == 6);
}

static void test_elements_need_the_arrays_type() {
    assert(!test_compiles("func f(): i32 {\n"
                          "    let xs = [1, 2, 3];\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_an_index_outside_the_array_fails_the_run() {
    assert(test_run_status("func f(): i32 {\n"
                           "    let xs: array<i32, 2>;\n"
                           "    let i: i32 = 2;\n"
                           "    return xs[i];\n"
                           "}\n"
                           "let r: i32 = f();") == VM_RUN_ERR_BOUNDS);

    assert(test_run_status("func f(): i32 {\n"
                           "    let xs: array<i32, 2>;\n"
                           "    let i: i32 = 0 - 1;\n"
                           "    return xs[i];\n"
                           "}\n"
                           "let r: i32 = f();") == VM_RUN_ERR_BOUNDS);
}

static void test_a_computed_index_is_checked_too() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 4>;\n"
                        "    for let i: i32 = 0; i < 4; i = i + 1 {\n"
                        "        xs[i] = i * i;\n"
                        "    }\n"
                        "    return xs[3];\n"
                        "}\n"
                        "let r: i32 = f();") == 9);
}

static void test_a_length_must_be_a_positive_literal() {
    assert(!test_compiles("func f(): i32 {\n"
                          "    let xs: array<i32, 0>;\n"
                          "    return 0;\n"
                          "}\n"));

    assert(!test_compiles("func f(): i32 {\n"
                          "    let n: i32 = 3;\n"
                          "    let xs: array<i32, n>;\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_an_array_too_wide_for_a_frame_is_refused() {
    assert(!test_compiles("func f(): i32 {\n"
                          "    let xs: array<i32, 1000>;\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_an_owning_element_is_freed_with_the_array() {
    assert(test_run_int("struct Box { n: i32 }\n"
                        "func f(): i32 {\n"
                        "    let xs: array<*Box, 2>;\n"
                        "    xs[0] = box Box { n: 0 };\n"
                        "    xs[1] = box Box { n: 0 };\n"
                        "    xs[1].n = 5;\n"
                        "    return xs[1].n;\n"
                        "}\n"
                        "let r: i32 = f();") == 5);
}

static void test_a_struct_element_keeps_its_own_slot() {
    assert(test_run_int("struct Point { x: i32, y: i32 }\n"
                        "func f(): i32 {\n"
                        "    let ps: array<Point, 2>;\n"
                        "    ps[0].x = 1;\n"
                        "    ps[1].x = 10;\n"
                        "    ps[1].y = 20;\n"
                        "    return ps[0].x + ps[1].x + ps[1].y;\n"
                        "}\n"
                        "let r: i32 = f();") == 31);
}

static void test_a_length_takes_no_argument() {
    assert(!test_compiles("func f(): i32 { let xs: array<i32, 4>; return xs.len(1); }\n"
                          "let r: i32 = f();"));
}

static void test_a_borrowed_array_knows_its_length() {
    assert(
        test_run_int("func f(): i32 { let xs: array<i32, 4>; let r: &array<i32, 4> = xs; return r.len(); }\n"
                     "let r: i32 = f();") == 4);
}

static void test_an_array_knows_its_length() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 4>;\n"
                        "    return xs.len();\n"
                        "}\n"
                        "let r: i32 = f();") == 4);
}

static void test_an_array_of_a_copyable_element_copies() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 2> = [9, 8];\n"
                        "    let ys: array<i32, 2> = xs;\n"
                        "    return ys[0];\n"
                        "}\n"
                        "let r: i32 = f();") == 9);
}

static void test_an_array_of_an_owning_element_transfers() {
    assert(!test_compiles("struct Box { n: i32 }\n"
                          "func f(): i32 {\n"
                          "    let xs: array<*Box, 1>;\n"
                          "    let ys: array<*Box, 1> = xs;\n"
                          "    let zs: array<*Box, 1> = xs;\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_an_array_names_its_element_and_its_length() {
    assert(!test_compiles("func f(xs: Array): i32 { return 0; }\n"));

    assert(!test_compiles("func f(xs: i32[]): i32 { return 0; }\n"));

    assert(test_compiles("func f(xs: &array<i32, 3>): i32 { return 0; }\n"));
}

static void test_an_array_lends_to_a_borrow() {
    assert(test_run_int("func g(xs: &array<i32, 3>): i32 { return xs[0]; }\n"
                        "func f(): i32 { let xs: array<i32, 3> = [4, 5, 6]; return g(xs); }\n"
                        "let r: i32 = f();") == 4);
}

static void test_what_may_be_indexed_and_by_what() {
    assert(!test_compiles("func f(): i32 {\n"
                          "    let n: i32 = 1;\n"
                          "    return n[0];\n"
                          "}\n"));

    assert(!test_compiles("func f(): i32 {\n"
                          "    let xs: array<i32, 1>;\n"
                          "    return xs[true];\n"
                          "}\n"));
}

static void test_the_brackets_say_what_the_length_counts() {
    assert(test_run_int("struct Cell { n: i32 }\n"
                        "func f(): i32 {\n"
                        "    let xs: array<*Cell, 2>;\n"
                        "    xs[0] = box Cell { n: 0 };\n"
                        "    xs[0].n = 5;\n"
                        "    return xs[0].n;\n"
                        "}\n"
                        "let r: i32 = f();") == 5);
}

static void test_the_bare_array_names_no_type() {
    assert(!test_compiles("func f(): i32 {\n"
                          "    let xs: Array;\n"
                          "    return 0;\n"
                          "}\n"
                          "let r: i32 = f();"));
}

static void test_indexing_an_array_given_up_is_refused() {
    assert(!test_compiles("struct Box { n: i32 }\n"
                          "func f(): i32 {\n"
                          "    let xs: array<*Box, 1>;\n"
                          "    let ys: array<*Box, 1> = xs;\n"
                          "    return xs[0].n;\n"
                          "}\n"));

    assert(!test_compiles("struct Box { n: i32 }\n"
                          "struct Key { n: i32, b: *Box }\n"
                          "func f(): i32 {\n"
                          "    let xs: array<i32, 2>;\n"
                          "    let k: Key;\n"
                          "    let j: Key = k;\n"
                          "    return xs[k.n];\n"
                          "}\n"));
}

static void test_giving_up_one_element_is_refused() {
    assert(!test_compiles("struct Box { n: i32 }\n"
                          "func f(): i32 {\n"
                          "    let xs: array<*Box, 2>;\n"
                          "    let b: *Box = xs[0];\n"
                          "    return 0;\n"
                          "}\n"));
}

int main(void) {
    test_a_fixed_array_holds_its_length_in_its_type();
    test_elements_are_distinct();
    test_an_array_starts_zeroed();
    test_the_elements_may_be_written_out();
    test_elements_may_be_written_wherever_the_type_is_known();
    test_elements_need_the_arrays_type();
    test_an_index_outside_the_array_fails_the_run();
    test_a_computed_index_is_checked_too();
    test_a_length_must_be_a_positive_literal();
    test_an_array_too_wide_for_a_frame_is_refused();
    test_an_owning_element_is_freed_with_the_array();
    test_a_struct_element_keeps_its_own_slot();
    test_a_length_takes_no_argument();
    test_a_borrowed_array_knows_its_length();
    test_an_array_knows_its_length();
    test_an_array_of_a_copyable_element_copies();
    test_an_array_of_an_owning_element_transfers();
    test_indexing_an_array_given_up_is_refused();
    test_giving_up_one_element_is_refused();
    test_an_array_names_its_element_and_its_length();
    test_an_array_lends_to_a_borrow();
    test_what_may_be_indexed_and_by_what();
    test_the_brackets_say_what_the_length_counts();
    test_the_bare_array_names_no_type();

    return 0;
}
