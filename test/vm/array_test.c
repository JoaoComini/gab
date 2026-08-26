#include "support/run.h"

// An array is a run of elements a header names: where they are and how many.
// Indexing reads and writes one.
static void test_an_element_is_read_by_its_index() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Array int = Array int[3];\n"
                        "    xs[1] = 7;\n"
                        "    return xs[1];\n"
                        "}\n"
                        "let r: int = f();") == 7);
}

// Each element is its own slot: writing one leaves its neighbours alone.
static void test_elements_are_distinct() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Array int = Array int[3];\n"
                        "    xs[0] = 1;\n"
                        "    xs[1] = 20;\n"
                        "    xs[2] = 300;\n"
                        "    return xs[0] + xs[1] + xs[2];\n"
                        "}\n"
                        "let r: int = f();") == 321);
}

// An index outside the array fails the run rather than reading whatever lies
// past the block.
static void test_an_index_outside_the_array_fails_the_run() {
    assert(test_run_status("func f(): int {\n"
                           "    let xs: Array int = Array int[2];\n"
                           "    return xs[2];\n"
                           "}\n"
                           "let r: int = f();") == VM_RUN_ERR_BOUNDS);

    assert(test_run_status("func f(): int {\n"
                           "    let xs: Array int = Array int[2];\n"
                           "    return xs[-1];\n"
                           "}\n"
                           "let r: int = f();") == VM_RUN_ERR_BOUNDS);
}

// A length is not known until it is computed, so the check is against the
// header rather than against anything the compiler saw.
static void test_a_computed_index_is_checked_too() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Array int = Array int[4];\n"
                        "    for let i: int = 0; i < 4; i = i + 1 {\n"
                        "        xs[i] = i * i;\n"
                        "    }\n"
                        "    return xs[3];\n"
                        "}\n"
                        "let r: int = f();") == 9);
}

// A negative length has no array to describe, so it fails where it is asked
// for rather than allocating something no index could reach.
static void test_a_negative_length_fails_the_run() {
    assert(test_run_status("func f(): int {\n"
                           "    let n: int = 0 - 1;\n"
                           "    let xs: Array int = Array int[n];\n"
                           "    return 0;\n"
                           "}\n"
                           "let r: int = f();") == VM_RUN_ERR_BOUNDS);
}

// An element that owns is freed with the array, which is what the count in the
// block's header is for: the type says how to free one element, and the count
// says how many were allocated.
static void test_an_owning_element_is_freed_with_the_array() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func f(): int {\n"
                        "    let xs: Array box Box = Array box Box[2];\n"
                        "    xs[0] = new Box;\n"
                        "    xs[1] = new Box;\n"
                        "    xs[1].n = 5;\n"
                        "    return xs[1].n;\n"
                        "}\n"
                        "let r: int = f();") == 5);
}

// A struct element is stored and read whole, at its own stride.
// A struct element is stored and read whole, at its own stride, so writing one
// leaves its neighbour's fields alone.
static void test_a_struct_element_keeps_its_own_slot() {
    assert(test_run_int("struct Point { x: int, y: int }\n"
                        "func f(): int {\n"
                        "    let ps: Array Point = Array Point[2];\n"
                        "    ps[0].x = 1;\n"
                        "    ps[1].x = 10;\n"
                        "    ps[1].y = 20;\n"
                        "    return ps[0].x + ps[1].x + ps[1].y;\n"
                        "}\n"
                        "let r: int = f();") == 31);
}

// How many elements an array holds is the count its header carries, which is
// what every bounds check reads.
static void test_an_array_knows_its_length() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Array int = Array int[4];\n"
                        "    return xs.len();\n"
                        "}\n"
                        "let r: int = f();") == 4);
}

// An array owns its elements, so a second binding to one would leave two slots
// believing they free the same block. Moving is how it changes hands.
static void test_an_array_does_not_copy() {
    assert(!test_compiles("func f(): int {\n"
                          "    let xs: Array int = Array int[1];\n"
                          "    let ys: Array int = xs;\n"
                          "    return 0;\n"
                          "}\n"));

    assert(test_run_int("func f(): int {\n"
                        "    let xs: Array int = Array int[2];\n"
                        "    xs[0] = 9;\n"
                        "    let ys: Array int = move xs;\n"
                        "    return ys[0];\n"
                        "}\n"
                        "let r: int = f();") == 9);
}

// 'Array' names a shape rather than a type: what it holds has to be written.
static void test_an_array_names_its_element() {
    assert(!test_compiles("func f(xs: Array): int { return 0; }\n"));

    assert(test_compiles("func f(xs: ref Array int): int { return 0; }\n"));
}

// An array lends to a borrow like anything else, and the borrow reaches the
// same header -- so the length is the one the caller allocated.
static void test_an_array_lends_to_a_borrow() {
    assert(test_run_int("func g(xs: ref Array int): int { return xs.len(); }\n"
                        "func f(): int { let xs: Array int = Array int[3]; return g(xs); }\n"
                        "let r: int = f();") == 3);
}

// Only an array is indexed, and only by an int.
static void test_what_may_be_indexed_and_by_what() {
    assert(!test_compiles("func f(): int {\n"
                          "    let n: int = 1;\n"
                          "    return n[0];\n"
                          "}\n"));

    assert(!test_compiles("func f(): int {\n"
                          "    let xs: Array int = Array int[1];\n"
                          "    return xs[true];\n"
                          "}\n"));
}

int main(void) {
    test_an_element_is_read_by_its_index();
    test_elements_are_distinct();
    test_an_index_outside_the_array_fails_the_run();
    test_a_computed_index_is_checked_too();
    test_a_negative_length_fails_the_run();
    test_an_owning_element_is_freed_with_the_array();
    test_a_struct_element_keeps_its_own_slot();
    test_an_array_knows_its_length();
    test_an_array_does_not_copy();
    test_an_array_names_its_element();
    test_an_array_lends_to_a_borrow();
    test_what_may_be_indexed_and_by_what();

    return 0;
}
