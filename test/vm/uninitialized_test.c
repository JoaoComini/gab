// A pointer slot read before anything is written to it names whatever the
// frame last left there, so reading one is refused. A slot that is not a
// pointer is left alone: unwritten ints are a wrong answer, not a wild read.

#include "support/run.h"

#include <assert.h>
#include <stdio.h>

// An owning pointer read before it is given a value would dereference whatever
// the slot held.
static void test_reading_an_uninitialized_owning_pointer_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: *Box;\n"
                          "    return a.n;\n"
                          "}\n"));
}

// A borrow is the same read through a different spelling, and nothing nulls a
// 'ref' slot, so it is refused too.
static void test_reading_an_uninitialized_borrow_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let b: ref Box;\n"
                          "    return b.n;\n"
                          "}\n"));
}

// Giving it a value first is what the rule asks for.
static void test_a_pointer_assigned_before_use_is_accepted() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: *Box;\n"
                        "    a = new Box;\n"
                        "    a.n = 5;\n"
                        "    return a.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

// A slot written on only one arm of an 'if' is not written on every path that
// reaches the use, so it is refused after the join.
static void test_a_pointer_assigned_on_one_arm_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: *Box;\n"
                          "    if 1 < 2 { a = new Box; }\n"
                          "    return a.n;\n"
                          "}\n"));
}

// Written on both arms, it holds a value whichever way control went.
static void test_a_pointer_assigned_on_both_arms_is_accepted() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: *Box;\n"
                        "    if 1 < 2 { a = new Box; } else { a = new Box; }\n"
                        "    a.n = 6;\n"
                        "    return a.n;\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

// A struct is not a pointer: its slots exist from the declaration, so building
// one field by field is how a struct is made rather than an error.
static void test_a_struct_built_field_by_field_is_accepted() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func main(): int {\n"
                        "    let p: Player;\n"
                        "    p.health = 100;\n"
                        "    return p.health;\n"
                        "}\n"
                        "let r: int = main();") == 100);
}

// Nor is a scalar. An unwritten int reads as whatever it reads as, which is a
// wrong answer rather than a wild pointer.
static void test_an_uninitialized_int_is_accepted() {
    assert(test_compiles("func main(): int {\n"
                         "    let n: int;\n"
                         "    return n;\n"
                         "}\n"));
}

int main(void) {
    test_reading_an_uninitialized_owning_pointer_is_refused();
    test_reading_an_uninitialized_borrow_is_refused();
    test_a_pointer_assigned_before_use_is_accepted();
    test_a_pointer_assigned_on_one_arm_is_refused();
    test_a_pointer_assigned_on_both_arms_is_accepted();
    test_a_struct_built_field_by_field_is_accepted();
    test_an_uninitialized_int_is_accepted();

    printf("All uninitialized use tests passed\n");
    return 0;
}
