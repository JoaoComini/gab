#include "support/run.h"

#include <assert.h>
#include <stdbool.h>

static void test_a_field_of_a_borrowed_parameter_is_returned_as_a_borrow() {
    assert(test_compiles("struct Bag { n: i32 }\n"
                         "func peek(b: &Bag): &i32 { return b.n; }\n"));
}

static void test_a_field_of_a_local_is_not_returned_as_a_borrow() {
    assert(!test_compiles("struct Bag { n: i32 }\n"
                          "func peek(): &i32 {\n"
                          "    let b = Bag { n: 1 };\n"
                          "    return b.n;\n"
                          "}\n"));
}

static void test_a_returned_borrow_reads_through_to_the_caller() {
    assert(test_run_int("struct Bag { n: i32 }\n"
                        "func peek(b: &Bag): &i32 { return b.n; }\n"
                        "func main(): i32 {\n"
                        "    let b = Bag { n: 7 };\n"
                        "    let r: &i32 = peek(b);\n"
                        "    return *r;\n"
                        "}\n"
                        "let r: i32 = main();") == 7);
}

static void test_a_nested_field_is_returned_as_a_borrow() {
    assert(test_compiles("struct Inner { m: i32 }\n"
                         "struct Bag { i: Inner }\n"
                         "func peek(b: &Bag): &Inner { return b.i; }\n"));
}

static void test_index_returns_a_borrow_of_its_element() {
    assert(test_run_int("func main(): i32 {\n"
                        "    let xs: array<i32, 2> = [4, 1];\n"
                        "    let e: &i32 = xs.index(0);\n"
                        "    return *e;\n"
                        "}\n"
                        "let r: i32 = main();") == 4);
}

static void test_index_borrows_rather_than_copying() {
    assert(test_run_int("func main(): i32 {\n"
                        "    let xs: array<i32, 2> = [1, 2];\n"
                        "    let e: &i32 = xs.index(0);\n"
                        "    xs[0] = 9;\n"
                        "    return *e;\n"
                        "}\n"
                        "let r: i32 = main();") == 9);
}

static void test_an_index_outside_the_container_fails_the_run() {
    assert(test_run_status("func main(): i32 {\n"
                           "    let xs: array<i32, 2> = [1, 2];\n"
                           "    let at: i32 = 3;\n"
                           "    return *xs.index(at);\n"
                           "}\n"
                           "let r: i32 = main();") != VM_RUN_OK);
}

int main(void) {
    test_a_field_of_a_borrowed_parameter_is_returned_as_a_borrow();
    test_a_field_of_a_local_is_not_returned_as_a_borrow();
    test_a_returned_borrow_reads_through_to_the_caller();
    test_a_nested_field_is_returned_as_a_borrow();
    test_index_returns_a_borrow_of_its_element();
    test_index_borrows_rather_than_copying();
    test_an_index_outside_the_container_fails_the_run();

    return 0;
}
