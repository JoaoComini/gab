#include "support/run.h"

#include <assert.h>
#include <stdbool.h>

static void test_a_field_of_a_borrowed_parameter_is_returned_as_a_borrow() {
    assert(test_compiles("struct Bag { n: int }\n"
                         "func peek(b: &Bag): &int { return b.n; }\n"));
}

static void test_a_field_of_a_local_is_not_returned_as_a_borrow() {
    assert(!test_compiles("struct Bag { n: int }\n"
                          "func peek(): &int {\n"
                          "    let b = Bag { n: 1 };\n"
                          "    return b.n;\n"
                          "}\n"));
}

static void test_a_returned_borrow_reads_through_to_the_caller() {
    assert(test_run_int("struct Bag { n: int }\n"
                        "func peek(b: &Bag): &int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let b = Bag { n: 7 };\n"
                        "    let r: &int = peek(b);\n"
                        "    return *r;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

static void test_a_nested_field_is_returned_as_a_borrow() {
    assert(test_compiles("struct Inner { m: int }\n"
                         "struct Bag { i: Inner }\n"
                         "func peek(b: &Bag): &Inner { return b.i; }\n"));
}

static void test_at_returns_a_borrow_of_its_element() {
    assert(test_run_int("import std;\n"
                        "func main(): int {\n"
                        "    let xs: Vec<int> = Vec<int>::new(0);\n"
                        "    xs.push(4);\n"
                        "    let e: &int = xs.index(0);\n"
                        "    return *e;\n"
                        "}\n"
                        "let r: int = main();") == 4);
}

static void test_at_borrows_rather_than_copying() {
    assert(test_run_int("import std;\n"
                        "func main(): int {\n"
                        "    let xs: Vec<int> = Vec<int>::new(0);\n"
                        "    xs.push(1);\n"
                        "    let e: &int = xs.index(0);\n"
                        "    xs.push(2);\n"
                        "    return xs.len();\n"
                        "}\n"
                        "let r: int = main();") == 2);
}

static void test_an_index_outside_the_vector_fails_the_run() {
    assert(test_run_status("import std;\n"
                           "func main(): int {\n"
                           "    let xs: Vec<int> = Vec<int>::new(0);\n"
                           "    xs.push(1);\n"
                           "    return *xs.index(3);\n"
                           "}\n"
                           "let r: int = main();") != VM_RUN_OK);
}

int main(void) {
    test_a_field_of_a_borrowed_parameter_is_returned_as_a_borrow();
    test_a_field_of_a_local_is_not_returned_as_a_borrow();
    test_a_returned_borrow_reads_through_to_the_caller();
    test_a_nested_field_is_returned_as_a_borrow();
    test_at_returns_a_borrow_of_its_element();
    test_at_borrows_rather_than_copying();
    test_an_index_outside_the_vector_fails_the_run();

    return 0;
}
