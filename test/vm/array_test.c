#include "support/run.h"

// A fixed array's length is part of its type: it is written where the type is,
// and every element lives in the array itself.
static void test_a_fixed_array_holds_its_length_in_its_type() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: [int; 3];\n"
                        "    xs[1] = 7;\n"
                        "    return xs[1];\n"
                        "}\n"
                        "let r: int = f();") == 7);
}

// Each element is its own slot: writing one leaves its neighbours alone.
static void test_elements_are_distinct() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: [int; 3];\n"
                        "    xs[0] = 1;\n"
                        "    xs[1] = 20;\n"
                        "    xs[2] = 300;\n"
                        "    return xs[0] + xs[1] + xs[2];\n"
                        "}\n"
                        "let r: int = f();") == 321);
}

// A declaration with nothing written into it is zeroed, as every allocation of
// a layout is.
static void test_an_array_starts_zeroed() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: [int; 3];\n"
                        "    return xs[0] + xs[1] + xs[2];\n"
                        "}\n"
                        "let r: int = f();") == 0);
}

// The elements may be written out, and there must be as many as the type says.
static void test_the_elements_may_be_written_out() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: [int; 3] = [1, 20, 300];\n"
                        "    return xs[0] + xs[1] + xs[2];\n"
                        "}\n"
                        "let r: int = f();") == 321);

    assert(!test_compiles("func f(): int {\n"
                          "    let xs: [int; 3] = [1, 2];\n"
                          "    return 0;\n"
                          "}\n"));

    assert(!test_compiles("func f(): int {\n"
                          "    let xs: [int; 2] = [1, true];\n"
                          "    return 0;\n"
                          "}\n"));
}

// Wherever the destination's type is known, the elements may be written out:
// what an array literal must be is what it is being stored into.
static void test_elements_may_be_written_wherever_the_type_is_known() {
    assert(test_run_int("func g(xs: [int; 2]): int { return xs[1]; }\n"
                        "func f(): int { return g([7, 8]); }\n"
                        "let r: int = f();") == 8);

    assert(test_run_int("func g(): [int; 2] { return [3, 4]; }\n"
                        "func f(): int { let xs: [int; 2] = g(); return xs[0]; }\n"
                        "let r: int = f();") == 3);

    assert(test_run_int("func f(): int {\n"
                        "    let xs: [int; 2];\n"
                        "    xs = [5, 6];\n"
                        "    return xs[1];\n"
                        "}\n"
                        "let r: int = f();") == 6);
}

// A list of values says nothing about how many an array holds, so the type has
// to be written where one is used.
static void test_elements_need_the_arrays_type() {
    assert(!test_compiles("func f(): int {\n"
                          "    let xs = [1, 2, 3];\n"
                          "    return 0;\n"
                          "}\n"));
}

// An index outside the array fails the run rather than reading whatever lies
// past it.
static void test_an_index_outside_the_array_fails_the_run() {
    assert(test_run_status("func f(): int {\n"
                           "    let xs: [int; 2];\n"
                           "    let i: int = 2;\n"
                           "    return xs[i];\n"
                           "}\n"
                           "let r: int = f();") == VM_RUN_ERR_BOUNDS);

    assert(test_run_status("func f(): int {\n"
                           "    let xs: [int; 2];\n"
                           "    let i: int = 0 - 1;\n"
                           "    return xs[i];\n"
                           "}\n"
                           "let r: int = f();") == VM_RUN_ERR_BOUNDS);
}

// The bound is the type's, so a computed index is checked against it.
static void test_a_computed_index_is_checked_too() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: [int; 4];\n"
                        "    for let i: int = 0; i < 4; i = i + 1 {\n"
                        "        xs[i] = i * i;\n"
                        "    }\n"
                        "    return xs[3];\n"
                        "}\n"
                        "let r: int = f();") == 9);
}

// A length that describes no array is refused where it is written.
static void test_a_length_must_be_a_positive_literal() {
    assert(!test_compiles("func f(): int {\n"
                          "    let xs: [int; 0];\n"
                          "    return 0;\n"
                          "}\n"));

    assert(!test_compiles("func f(): int {\n"
                          "    let n: int = 3;\n"
                          "    let xs: [int; n];\n"
                          "    return 0;\n"
                          "}\n"));
}

// The elements have to be reachable by the operand that addresses one, so a run
// wider than that is refused where the type is written.
static void test_an_array_too_wide_for_a_frame_is_refused() {
    assert(!test_compiles("func f(): int {\n"
                          "    let xs: [int; 1000];\n"
                          "    return 0;\n"
                          "}\n"));
}

// An element that owns is freed with the array, one element at a time.
static void test_an_owning_element_is_freed_with_the_array() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func f(): int {\n"
                        "    let xs: [box Box; 2];\n"
                        "    xs[0] = new Box;\n"
                        "    xs[1] = new Box;\n"
                        "    xs[1].n = 5;\n"
                        "    return xs[1].n;\n"
                        "}\n"
                        "let r: int = f();") == 5);
}

// A struct element is stored and read whole, at its own stride, so writing one
// leaves its neighbour's fields alone.
static void test_a_struct_element_keeps_its_own_slot() {
    assert(test_run_int("struct Point { x: int, y: int }\n"
                        "func f(): int {\n"
                        "    let ps: [Point; 2];\n"
                        "    ps[0].x = 1;\n"
                        "    ps[1].x = 10;\n"
                        "    ps[1].y = 20;\n"
                        "    return ps[0].x + ps[1].x + ps[1].y;\n"
                        "}\n"
                        "let r: int = f();") == 31);
}

// How many elements an array holds is what its type says, so the call answers
// without reading anything.
static void test_an_array_knows_its_length() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: [int; 4];\n"
                        "    return xs.len();\n"
                        "}\n"
                        "let r: int = f();") == 4);
}

// An array of a copyable element copies, since it owns nothing that two slots
// could both free.
static void test_an_array_of_a_copyable_element_copies() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: [int; 2] = [9, 8];\n"
                        "    let ys: [int; 2] = xs;\n"
                        "    return ys[0];\n"
                        "}\n"
                        "let r: int = f();") == 9);
}

// An array of an owning element does not: a second binding would leave two
// slots believing they free the same objects.
static void test_an_array_of_an_owning_element_does_not_copy() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func f(): int {\n"
                          "    let xs: [box Box; 1];\n"
                          "    let ys: [box Box; 1] = xs;\n"
                          "    return 0;\n"
                          "}\n"));
}

// 'Array' names a shape rather than a type: what it holds and how many have to
// be written.
static void test_an_array_names_its_element_and_its_length() {
    assert(!test_compiles("func f(xs: Array): int { return 0; }\n"));

    assert(!test_compiles("func f(xs: int[]): int { return 0; }\n"));

    assert(test_compiles("func f(xs: ref [int; 3]): int { return 0; }\n"));
}

// An array lends to a borrow like anything else, and the borrow indexes what
// the caller holds.
static void test_an_array_lends_to_a_borrow() {
    assert(test_run_int("func g(xs: ref [int; 3]): int { return xs[0]; }\n"
                        "func f(): int { let xs: [int; 3] = [4, 5, 6]; return g(xs); }\n"
                        "let r: int = f();") == 4);
}

// Only an array is indexed, and only by an int.
static void test_what_may_be_indexed_and_by_what() {
    assert(!test_compiles("func f(): int {\n"
                          "    let n: int = 1;\n"
                          "    return n[0];\n"
                          "}\n"));

    assert(!test_compiles("func f(): int {\n"
                          "    let xs: [int; 1];\n"
                          "    return xs[true];\n"
                          "}\n"));
}

// The element is what the brackets hold, so a prefix inside them is part of the
// element and one outside them is applied to the run: '[box T; n]' is a run of
// pointers, and 'box [T; n]' is a pointer to a run.
static void test_the_brackets_say_what_the_length_counts() {
    assert(test_run_int("struct Cell { n: int }\n"
                        "func f(): int {\n"
                        "    let xs: [box Cell; 2];\n"
                        "    xs[0] = new Cell;\n"
                        "    xs[0].n = 5;\n"
                        "    return xs[0].n;\n"
                        "}\n"
                        "let r: int = f();") == 5);
}

// 'Array' names a declaration rather than a type: what it takes is what makes
// it one, so a mention supplying nothing has no type to be.
static void test_the_bare_array_names_no_type() {
    assert(!test_compiles("func f(): int {\n"
                          "    let xs: Array;\n"
                          "    return 0;\n"
                          "}\n"
                          "let r: int = f();"));
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
    test_an_array_knows_its_length();
    test_an_array_of_a_copyable_element_copies();
    test_an_array_of_an_owning_element_does_not_copy();
    test_an_array_names_its_element_and_its_length();
    test_an_array_lends_to_a_borrow();
    test_what_may_be_indexed_and_by_what();
    test_the_brackets_say_what_the_length_counts();
    test_the_bare_array_names_no_type();

    return 0;
}
