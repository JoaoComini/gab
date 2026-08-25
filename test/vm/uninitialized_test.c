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
                          "    let a: box Box;\n"
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
                        "    let a: box Box;\n"
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
                          "    let a: box Box;\n"
                          "    if 1 < 2 { a = new Box; }\n"
                          "    return a.n;\n"
                          "}\n"));
}

// Written on both arms, it holds a value whichever way control went.
static void test_a_pointer_assigned_on_both_arms_is_accepted() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: box Box;\n"
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

// An owning field is nulled when the struct holding it is declared, so
// reaching through one before anything is stored in it dereferences null.
static void test_reading_through_an_unwritten_owning_field_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Holder { b: box Box }\n"
                          "func main(): int {\n"
                          "    let h: Holder;\n"
                          "    return h.b.n;\n"
                          "}\n"));
}

// Storing into the field first is what the rule asks for, and the field is
// readable from there on.
static void test_an_owning_field_written_before_use_is_accepted() {
    assert(test_run_int("struct Box { n: int }\n"
                        "struct Holder { b: box Box }\n"
                        "func main(): int {\n"
                        "    let h: Holder;\n"
                        "    h.b = new Box;\n"
                        "    h.b.n = 9;\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// A field written on only one arm of an 'if' is not written on every path
// reaching the use, so it is refused after the join.
static void test_an_owning_field_written_on_one_arm_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Holder { b: box Box }\n"
                          "func main(): int {\n"
                          "    let h: Holder;\n"
                          "    if 1 < 2 { h.b = new Box; }\n"
                          "    return h.b.n;\n"
                          "}\n"));
}

// The written-field set names a struct's owning fields, so what fills it is
// how many of those a struct has rather than where they sit. A struct padded
// out with scalars is tracked on the owning field past them exactly as it
// would be on the first.
static void test_an_owning_field_past_the_scalars_is_still_tracked() {
    char source[4096];
    int written = snprintf(source, sizeof source, "struct Box { n: int }\nstruct Wide {\n");

    for (int i = 0; i < 100; i++) {
        written += snprintf(source + written, sizeof source - (size_t)written, "    f%d: int,\n", i);
    }

    snprintf(source + written, sizeof source - (size_t)written,
             "    b: box Box\n"
             "}\n"
             "func main(): int {\n"
             "    let w: Wide;\n"
             "    return w.b.n;\n"
             "}\n");

    assert(!test_compiles(source));
}

// Each owning field gets its own bit, so writing one says nothing about
// another: the second is still unwritten after the first is stored into.
static void test_writing_one_owning_field_leaves_the_other_unwritten() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Pair { a: box Box, b: box Box }\n"
                          "func main(): int {\n"
                          "    let p: Pair;\n"
                          "    p.a = new Box;\n"
                          "    return p.b.n;\n"
                          "}\n"));

    assert(test_run_int("struct Box { n: int }\n"
                        "struct Pair { a: box Box, b: box Box }\n"
                        "func main(): int {\n"
                        "    let p: Pair;\n"
                        "    p.a = new Box;\n"
                        "    p.b = new Box;\n"
                        "    p.a.n = 2;\n"
                        "    p.b.n = 3;\n"
                        "    return p.a.n + p.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

int main(void) {
    test_reading_an_uninitialized_owning_pointer_is_refused();
    test_reading_an_uninitialized_borrow_is_refused();
    test_a_pointer_assigned_before_use_is_accepted();
    test_a_pointer_assigned_on_one_arm_is_refused();
    test_a_pointer_assigned_on_both_arms_is_accepted();
    test_a_struct_built_field_by_field_is_accepted();
    test_an_uninitialized_int_is_accepted();
    test_reading_through_an_unwritten_owning_field_is_refused();
    test_an_owning_field_written_before_use_is_accepted();
    test_an_owning_field_written_on_one_arm_is_refused();
    test_an_owning_field_past_the_scalars_is_still_tracked();
    test_writing_one_owning_field_leaves_the_other_unwritten();

    printf("All uninitialized use tests passed\n");
    return 0;
}
