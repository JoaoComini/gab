// Copying is implicit and is the default; moving is explicit and is required
// for anything that owns. What a type holds decides which it gets, and a slot
// moved out of is dead.

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
// owning pointer owns transitively, so it is not copyable.
static void test_a_type_holding_an_owning_pointer_does_not_copy() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "struct Holder { b: *Box }\n"
                          "func main(): int {\n"
                          "    let h: Holder;\n"
                          "    let other = h;\n"
                          "    return 0;\n"
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

// Ownership transfers where 'move' says it does.
static void test_move_transfers_ownership() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: *Box = new Box;\n"
                        "    a.n = 7;\n"
                        "    let b = move a;\n"
                        "    return b.n;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

// The slot moved out of is dead: it no longer names anything, so reading it is
// an error rather than a second owner of the same object.
static void test_a_moved_from_slot_is_dead() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: *Box = new Box;\n"
                          "    let b = move a;\n"
                          "    return a.n;\n"
                          "}\n"));
}

// Assigning something longer-lived to a dead slot brings it back: deadness is
// about what the slot holds, not a mark the name carries forever.
static void test_assigning_to_a_dead_slot_revives_it() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: *Box = new Box;\n"
                        "    let b = move a;\n"
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
                          "    let a: *Box = new Box;\n"
                          "    if 1 < 2 { let b = move a; } else { }\n"
                          "    return a.n;\n"
                          "}\n"));
}

// Moving in a loop body would move the same slot on the second iteration, so
// the back-edge makes the move itself the error.
static void test_moving_the_same_slot_each_iteration_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: *Box = new Box;\n"
                          "    for let i = 0; i < 2; i = i + 1 {\n"
                          "        let b = move a;\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

// Writing an implicit copy of a non-copyable value is an error whose message
// names the way out and says why the other one is unavailable.
static void test_the_implicit_copy_error_names_the_remedies() {
    const char *source = "struct Box { n: int }\n"
                         "func main(): int {\n"
                         "    let a: *Box = new Box;\n"
                         "    let b = a;\n"
                         "    return 0;\n"
                         "}\n";

    assert(!test_compiles(source));
    assert(test_diagnostic_mentions(source, "move"));
    assert(test_diagnostic_mentions(source, "declares no 'clone'"));
}

int main(void) {
    test_a_type_owning_nothing_copies_implicitly();
    test_a_type_holding_an_owning_pointer_does_not_copy();
    test_a_type_holding_a_borrow_still_copies();
    test_move_transfers_ownership();
    test_a_moved_from_slot_is_dead();
    test_assigning_to_a_dead_slot_revives_it();
    test_a_slot_moved_on_one_arm_is_dead_after_the_join();
    test_moving_the_same_slot_each_iteration_is_refused();
    test_the_implicit_copy_error_names_the_remedies();

    printf("All move tests passed\n");
    return 0;
}
