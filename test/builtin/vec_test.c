#include "support/run.h"

static void test_a_vec_holds_what_is_pushed() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Vec<int>;\n"
                        "    xs.push(7);\n"
                        "    return xs.at(0);\n"
                        "}\n"
                        "let r: int = f();") == 7);
}

static void test_a_vec_grows_past_its_first_block() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Vec<int>;\n"
                        "    for let i: int = 0; i < 10; i = i + 1 { xs.push(i); }\n"
                        "    return xs.at(0) + xs.at(9);\n"
                        "}\n"
                        "let r: int = f();") == 9);
}

static void test_a_vec_counts_what_it_holds() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Vec<int>;\n"
                        "    for let i: int = 0; i < 10; i = i + 1 { xs.push(i); }\n"
                        "    return xs.len();\n"
                        "}\n"
                        "let r: int = f();") == 10);
}

static void test_an_index_outside_the_vec_fails_the_run() {
    assert(test_run_status("func f(): int {\n"
                           "    let xs: Vec<int>;\n"
                           "    xs.push(1);\n"
                           "    return xs.at(1);\n"
                           "}\n"
                           "let r: int = f();") == VM_RUN_ERR_EXTERN);
}

static void test_an_owning_element_is_freed_with_the_vec() {
    assert(test_run_int("struct Cell { value: int }\n"
                        "func f(): int {\n"
                        "    let xs: Vec<box Cell>;\n"
                        "    for let i: int = 0; i < 10; i = i + 1 { xs.push(new Cell); }\n"
                        "    return xs.len();\n"
                        "}\n"
                        "let r: int = f();") == 10);
}

static void test_a_vec_transfers_rather_than_copying() {
    assert(!test_codegens("func f(): int {\n"
                          "    let xs: Vec<int>;\n"
                          "    let ys: Vec<int> = xs;\n"
                          "    return xs.len();\n"
                          "}\n"
                          "let r: int = f();"));
}

static void test_a_vec_needs_an_element() {
    assert(!test_compiles("func f(): int { let xs: Vec; return 0; }"));
}

static void test_a_vec_takes_one_element() {
    assert(!test_compiles("func f(): int { let xs: Vec<int,float>; return 0; }"));
}

static void test_a_push_yields_nothing() {
    assert(!test_compiles("func f(): int {\n"
                          "    let xs: Vec<int>;\n"
                          "    let n: int = xs.push(1);\n"
                          "    return n;\n"
                          "}\n"
                          "let r: int = f();"));
}

static void test_a_vec_holds_a_wider_element() {
    assert(test_run_int("struct Pair { a: int, b: int }\n"
                        "func f(): int {\n"
                        "    let xs: Vec<Pair>;\n"
                        "    let p: Pair;\n"
                        "    p.a = 3; p.b = 4;\n"
                        "    xs.push(p);\n"
                        "    p.a = 5; p.b = 6;\n"
                        "    xs.push(p);\n"
                        "    return xs.at(0).b + xs.at(1).a;\n"
                        "}\n"
                        "let r: int = f();") == 9);
}

static void test_a_push_takes_the_element_it_was_given() {
    assert(!test_compiles("func f(): int {\n"
                          "    let xs: Vec<int>;\n"
                          "    xs.push(true);\n"
                          "    return 0;\n"
                          "}\n"
                          "let r: int = f();"));
}

static void test_a_vec_holds_a_vec() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Vec<Vec<int>>;\n"
                        "    return xs.len();\n"
                        "}\n"
                        "let r: int = f();") == 0);
}

int main(void) {
    test_a_vec_holds_what_is_pushed();
    test_a_vec_grows_past_its_first_block();
    test_a_vec_counts_what_it_holds();
    test_an_index_outside_the_vec_fails_the_run();
    test_an_owning_element_is_freed_with_the_vec();
    test_a_vec_transfers_rather_than_copying();
    test_a_vec_needs_an_element();
    test_a_vec_takes_one_element();
    test_a_push_yields_nothing();
    test_a_vec_holds_a_wider_element();
    test_a_push_takes_the_element_it_was_given();
    test_a_vec_holds_a_vec();

    return 0;
}
