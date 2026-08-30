#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_a_type_owning_nothing_copies_implicitly() {
    assert(test_run_int("struct Point { x: int, y: int }\n"
                        "func main(): int {\n"
                        "    let a: Point;\n"
                        "    a.x = 3;\n"
                        "    let b = a;\n"
                        "    return b.x;\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

static void test_a_type_holding_an_owning_pointer_transfers() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Holder { b: box Box }\n"
                          "func main(): int {\n"
                          "    let h: Holder;\n"
                          "    h.b = new Box;\n"
                          "    let other = h;\n"
                          "    return h.b.n;\n"
                          "}\n"));
}

static void test_a_type_holding_a_borrow_still_copies() {
    assert(test_compiles("struct Box { n: int }\n"
                         "struct Watcher { b: ref Box }\n"
                         "func main(): int {\n"
                         "    let w: Watcher;\n"
                         "    let other = w;\n"
                         "    return 0;\n"
                         "}\n"));
}

static void test_binding_an_owning_value_transfers_it() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: box Box = new Box;\n"
                        "    a.n = 7;\n"
                        "    let b = a;\n"
                        "    return b.n;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

static void test_a_moved_from_slot_is_dead() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: box Box = new Box;\n"
                          "    let b = a;\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_assigning_to_a_dead_slot_revives_it() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: box Box = new Box;\n"
                        "    let b = a;\n"
                        "    a = new Box;\n"
                        "    a.n = 5;\n"
                        "    return a.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

static void test_a_slot_moved_on_one_arm_is_dead_after_the_join() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: box Box = new Box;\n"
                          "    if 1 < 2 { let b = a; } else { }\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_transferring_the_same_slot_each_iteration_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: box Box = new Box;\n"
                          "    for let i = 0; i < 2; i = i + 1 {\n"
                          "        let b = a;\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_reading_a_transferred_slot_names_the_slot() {
    const char *source = "struct Box { n: int }\n"
                         "func main(): int {\n"
                         "    let a: box Box = new Box;\n"
                         "    let b = a;\n"
                         "    return a.n;\n"
                         "}\n";

    assert(!test_compiles(source));
    assert(test_diagnostic_mentions(source, "no longer holds a value"));
}

int main(void) {
    test_a_type_owning_nothing_copies_implicitly();
    test_a_type_holding_an_owning_pointer_transfers();
    test_a_type_holding_a_borrow_still_copies();
    test_binding_an_owning_value_transfers_it();
    test_a_moved_from_slot_is_dead();
    test_assigning_to_a_dead_slot_revives_it();
    test_a_slot_moved_on_one_arm_is_dead_after_the_join();
    test_transferring_the_same_slot_each_iteration_is_refused();
    test_reading_a_transferred_slot_names_the_slot();

    printf("All tests passed\n");
    return 0;
}
