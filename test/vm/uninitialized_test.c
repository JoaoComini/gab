#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_reading_an_uninitialized_owning_pointer_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: box Box;\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_reading_an_uninitialized_borrow_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let b: ref Box;\n"
                          "    return b.n;\n"
                          "}\n"));
}

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

static void test_a_pointer_assigned_on_one_arm_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: box Box;\n"
                          "    if 1 < 2 { a = new Box; }\n"
                          "    return a.n;\n"
                          "}\n"));
}

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

static void test_a_struct_built_field_by_field_is_accepted() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func main(): int {\n"
                        "    let p: Player;\n"
                        "    p.health = 100;\n"
                        "    return p.health;\n"
                        "}\n"
                        "let r: int = main();") == 100);
}

static void test_an_uninitialized_int_is_accepted() {
    assert(test_compiles("func main(): int {\n"
                         "    let n: int;\n"
                         "    return n;\n"
                         "}\n"));
}

static void test_reading_through_an_unwritten_owning_field_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Holder { b: box Box }\n"
                          "func main(): int {\n"
                          "    let h: Holder;\n"
                          "    return h.b.n;\n"
                          "}\n"));
}

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

static void test_an_owning_field_written_on_one_arm_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Holder { b: box Box }\n"
                          "func main(): int {\n"
                          "    let h: Holder;\n"
                          "    if 1 < 2 { h.b = new Box; }\n"
                          "    return h.b.n;\n"
                          "}\n"));
}

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
