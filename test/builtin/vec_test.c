#include "support/run.h"

// A vector holds what has been pushed into it: how many is a fact about the
// value rather than about its type, which is what separates it from an array.
static void test_a_vec_holds_what_is_pushed() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Vec<int>;\n"
                        "    xs.push(7);\n"
                        "    return xs.at(0);\n"
                        "}\n"
                        "let r: int = f();") == 7);
}

// A vector grows: it holds more than the block it first allocated had room for,
// and the elements written before a growth survive it.
static void test_a_vec_grows_past_its_first_block() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Vec<int>;\n"
                        "    for let i: int = 0; i < 10; i = i + 1 { xs.push(i); }\n"
                        "    return xs.at(0) + xs.at(9);\n"
                        "}\n"
                        "let r: int = f();") == 9);
}

// The length counts what was pushed, however many blocks that took.
static void test_a_vec_counts_what_it_holds() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: Vec<int>;\n"
                        "    for let i: int = 0; i < 10; i = i + 1 { xs.push(i); }\n"
                        "    return xs.len();\n"
                        "}\n"
                        "let r: int = f();") == 10);
}

// An index past the live elements is not a value the caller could tell from
// one, so reaching for it fails the run.
static void test_an_index_outside_the_vec_fails_the_run() {
    assert(test_run_status("func f(): int {\n"
                           "    let xs: Vec<int>;\n"
                           "    xs.push(1);\n"
                           "    return xs.at(1);\n"
                           "}\n"
                           "let r: int = f();") == VM_RUN_ERR_EXTERN);
}

// An element that owns is freed with the vector holding it: the live ones are
// dropped, and then the block they sat in.
static void test_an_owning_element_is_freed_with_the_vec() {
    assert(test_run_int("struct Cell { value: int }\n"
                        "func f(): int {\n"
                        "    let xs: Vec<box Cell>;\n"
                        "    for let i: int = 0; i < 10; i = i + 1 { xs.push(new Cell); }\n"
                        "    return xs.len();\n"
                        "}\n"
                        "let r: int = f();") == 10);
}

// A vector owns its block, so copying one would make a second header freeing
// the same memory.
static void test_a_vec_transfers_rather_than_copying() {
    assert(!test_codegens("func f(): int {\n"
                          "    let xs: Vec<int>;\n"
                          "    let ys: Vec<int> = xs;\n"
                          "    return xs.len();\n"
                          "}\n"
                          "let r: int = f();"));
}

// 'Vec' alone names no type: what it holds is what makes it one.
static void test_a_vec_needs_an_element() {
    assert(!test_compiles("func f(): int { let xs: Vec; return 0; }"));
}

// One element and no more, which is what the declaration takes.
static void test_a_vec_takes_one_element() {
    assert(!test_compiles("func f(): int { let xs: Vec<int,float>; return 0; }"));
}

// A push is done for what it leaves behind, so there is no value to bind.
static void test_a_push_yields_nothing() {
    assert(!test_compiles("func f(): int {\n"
                          "    let xs: Vec<int>;\n"
                          "    let n: int = xs.push(1);\n"
                          "    return n;\n"
                          "}\n"
                          "let r: int = f();"));
}

// The element is what the instantiation was given, so a wider one strides by
// its own width.
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

// An element of one type is not an element of another: what a push takes is
// what the vector was instantiated over.
static void test_a_push_takes_the_element_it_was_given() {
    assert(!test_compiles("func f(): int {\n"
                          "    let xs: Vec<int>;\n"
                          "    xs.push(true);\n"
                          "    return 0;\n"
                          "}\n"
                          "let r: int = f();"));
}

// An argument is a type like any other, so one instantiation may be another's
// element.
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
