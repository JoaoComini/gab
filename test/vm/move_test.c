// Binding a value either copies it or hands its ownership over, and which one
// happens is read off the type rather than written at the site. What a type
// holds decides which it gets, and a slot moved out of is dead.

#include "support/run.h"

#include <assert.h>
#include <stdio.h>

// A type holding nothing that owns is copyable, so a second binding of it is
// an ordinary implicit copy.
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

// Copyability is derived from the type, not declared: a struct holding an
// owning pointer owns transitively, so binding it transfers rather than
// duplicates -- and the slot it came from is dead.
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

// A borrow owns nothing, so a struct holding one stays copyable.
static void test_a_type_holding_a_borrow_still_copies() {
    assert(test_compiles("struct Box { n: int }\n"
                         "struct Watcher { b: ref Box }\n"
                         "func main(): int {\n"
                         "    let w: Watcher;\n"
                         "    let other = w;\n"
                         "    return 0;\n"
                         "}\n"));
}

// Binding a value that owns hands the object over: what the new slot names is
// what the old one held, rather than a second copy of it.
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

// The slot moved out of is dead: it no longer names anything, so reading it is
// an error rather than a second owner of the same object.
static void test_a_moved_from_slot_is_dead() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: box Box = new Box;\n"
                          "    let b = a;\n"
                          "    return a.n;\n"
                          "}\n"));
}

// Assigning something longer-lived to a dead slot brings it back: deadness is
// about what the slot holds, not a mark the name carries forever.
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

// A slot moved on only one arm of an 'if' is dead after the join: it is live
// only if it is live on every path reaching the use.
static void test_a_slot_moved_on_one_arm_is_dead_after_the_join() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: box Box = new Box;\n"
                          "    if 1 < 2 { let b = a; } else { }\n"
                          "    return a.n;\n"
                          "}\n"));
}

// Transferring out of a slot in a loop body would empty it again on the second
// iteration, so the back-edge is what makes the bind an error.
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

// Reading a slot after it has been bound elsewhere names what it no longer
// holds, and the message says so rather than reporting the bind that emptied
// it.
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
