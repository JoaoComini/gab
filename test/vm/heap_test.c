#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>

#include <stdio.h>

// 'new T' yields a '*T' into the heap: writing through it and reading back is
// what proves the allocation is real and the pointer addresses the payload.
static void test_new_allocates_a_usable_object() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func main(): int {\n"
                        "    let p: *Player = new Player;\n"
                        "    p.health = 42;\n"
                        "    return p.health;\n"
                        "}\n"
                        "let r: int = main();") == 42);
}

// A fresh object is zeroed, so a field nobody assigned reads as 0 rather than
// whatever the allocator left behind. Release depends on this for pointers.
static void test_new_zeroes_the_payload() {
    assert(test_run_int("struct Player { health: int, mana: int }\n"
                        "func main(): int {\n"
                        "    let p: *Player = new Player;\n"
                        "    return p.health + p.mana;\n"
                        "}\n"
                        "let r: int = main();") == 0);
}

// A heap pointer outlives every frame, so returning one is allowed where
// returning '&local' is not.
static void test_a_heap_pointer_may_be_returned() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func make(): *Box {\n"
                        "    let b: *Box = new Box;\n"
                        "    b.n = 7;\n"
                        "    return b;\n"
                        "}\n"
                        "func main(): int { let b: *Box = make(); return b.n; }\n"
                        "let r: int = main();") == 7);
}

// Two allocations of the same type share one entry in the VM's type list, and
// two objects that do not alias.
static void test_two_objects_are_distinct() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: *Box = new Box;\n"
                        "    let b: *Box = new Box;\n"
                        "    a.n = 1;\n"
                        "    b.n = 2;\n"
                        "    return a.n * 10 + b.n;\n"
                        "}\n"
                        "let r: int = main();") == 12);
}

// A method on a pointer receiver is called through a heap pointer with no
// address-taking, since the receiver already is one.
static void test_method_on_a_heap_object() {
    assert(
        test_run_int("struct Player { health: int }\n"
                     "func (p: ref Player) hurt(n: int): int { p.health = p.health - n; return p.health; }\n"
                     "func main(): int {\n"
                     "    let p: *Player = new Player;\n"
                     "    p.health = 50;\n"
                     "    return p.hurt(8);\n"
                     "}\n"
                     "let r: int = main();") == 42);
}

// A heap object holding a pointer to another: the field is a '*T' like any
// other, which is what release will later walk.
static void test_an_object_can_hold_another() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { child: *Inner }\n"
                        "func main(): int {\n"
                        "    let o: *Outer = new Outer;\n"
                        "    o.child = new Inner;\n"
                        "    o.child.n = 9;\n"
                        "    return o.child.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// Two names for one object. The second is borrowed — the first still owns the
// reference — so releasing both would free it twice.
static void test_an_alias_is_borrowed_not_owned() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: *Box = new Box;\n"
                        "    a.n = 5;\n"
                        "    let b: *Box = a;\n"
                        "    return b.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

// Freed where its block closes, not where the frame pops: destruction is
// deterministic at the brace rather than whenever a collector runs.
static void test_released_at_the_end_of_its_block() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let t: int = 0;\n"
                        "    { let a: *Box = new Box; a.n = 3; t = a.n; }\n"
                        "    return t;\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

// Sibling blocks reuse slots, so the second block's int lands where the first
// block's pointer was. Releasing at the close rather than at the pop is what
// stops an integer being released as an object.
static void test_a_reused_slot_is_not_released_twice() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    { let a: *Box = new Box; a.n = 1; }\n"
                        "    { let x: int = 7; let y: int = 8; return x + y; }\n"
                        "}\n"
                        "let r: int = main();") == 15);
}

// Nothing ever stores it, so the statement is where it dies.
static void test_a_bare_new_does_not_leak() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int { new Box; return 4; }\n"
                        "let r: int = main();") == 4);
}

// A strong field owns what it names, so it may only be given something nothing
// else owns. 'i' is owned by its own slot, and storing it would leave the object
// with two owners — the field and the slot — each releasing it independently.
static void test_storing_a_borrowed_reference_into_a_field_is_refused() {
    assert(!test_codegens("struct Inner { n: int }\n"
                          "struct Outer { child: *Inner }\n"
                          "func main(): int {\n"
                          "    let o: *Outer = new Outer;\n"
                          "    let i: *Inner = new Inner;\n"
                          "    i.n = 6;\n"
                          "    o.child = i;\n"
                          "    return o.child.n;\n"
                          "}\n"
                          "let r: int = main();"));
}

// A parameter is a borrow like any other name, so it is refused for the same
// reason. This is the case that would otherwise outlive its caller's ownership.
static void test_storing_a_parameter_into_a_field_is_refused() {
    assert(!test_codegens("struct Inner { n: int }\n"
                          "struct Outer { child: *Inner }\n"
                          "func adopt(o: ref Outer, i: ref Inner): int {\n"
                          "    o.child = i;\n"
                          "    return 0;\n"
                          "}\n"
                          "let r: int = 0;"));
}

// The other half of the rule: an expression that hands its result over is
// exactly what a field may be given, and a call is one.
static void test_storing_a_call_result_into_a_field_is_allowed() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { child: *Inner }\n"
                        "func make(): *Inner {\n"
                        "    let i: *Inner = new Inner;\n"
                        "    i.n = 6;\n"
                        "    return i;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let o: *Outer = new Outer;\n"
                        "    o.child = make();\n"
                        "    return o.child.n;\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

// Overwriting a field that already holds a reference drops the old one, or the
// first object leaks with nothing left naming it.
static void test_overwriting_a_field_releases_the_old_value() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { child: *Inner }\n"
                        "func main(): int {\n"
                        "    let o: *Outer = new Outer;\n"
                        "    o.child = new Inner;\n"
                        "    o.child.n = 1;\n"
                        "    o.child = new Inner;\n"
                        "    o.child.n = 2;\n"
                        "    return o.child.n;\n"
                        "}\n"
                        "let r: int = main();") == 2);
}

// Reassigning a variable that owns a reference drops the old one too.
static void test_reassigning_a_variable_releases_the_old_value() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let p: *Box = new Box;\n"
                        "    p.n = 1;\n"
                        "    p = new Box;\n"
                        "    p.n = 9;\n"
                        "    return p.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// 'o.child = o.child' was the case that forced the retain-new, store,
// release-old order. A field may now only be given something unowned, and a
// field read is a borrow, so the program the ordering existed to survive is
// refused before it runs — which is the ordering problem going away rather than
// being solved.
static void test_field_self_assignment_is_refused() {
    assert(!test_codegens("struct Inner { n: int }\n"
                          "struct Outer { child: *Inner }\n"
                          "func main(): int {\n"
                          "    let o: *Outer = new Outer;\n"
                          "    o.child = new Inner;\n"
                          "    o.child.n = 5;\n"
                          "    o.child = o.child;\n"
                          "    return o.child.n;\n"
                          "}\n"
                          "let r: int = main();"));
}

// An owning variable may only be given something unowned, exactly as a field
// may. 'p = p' reads as a borrow like any other variable read, so it is refused
// with the rest — self-assignment needs no special handling when the assignment
// itself is not expressible.
static void test_variable_self_assignment_is_refused() {
    assert(!test_codegens("struct Box { n: int }\n"
                          "func main(): int { let p: *Box = new Box; p.n = 3; p = p; return p.n; }\n"
                          "let r: int = main();"));
}

// Reassigning an owning variable with a fresh object is the legal shape, and it
// frees what the slot held before.
static void test_reassigning_an_owning_variable_frees_the_old_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let p: *Box = new Box;\n"
                        "    p.n = 1;\n"
                        "    p = new Box;\n"
                        "    p.n = 9;\n"
                        "    return p.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

int main(void) {
    test_new_allocates_a_usable_object();
    test_overwriting_a_field_releases_the_old_value();
    test_reassigning_a_variable_releases_the_old_value();
    test_field_self_assignment_is_refused();
    test_variable_self_assignment_is_refused();
    test_reassigning_an_owning_variable_frees_the_old_object();
    test_storing_a_borrowed_reference_into_a_field_is_refused();
    test_storing_a_parameter_into_a_field_is_refused();
    test_storing_a_call_result_into_a_field_is_allowed();
    test_an_alias_is_borrowed_not_owned();
    test_released_at_the_end_of_its_block();
    test_a_reused_slot_is_not_released_twice();
    test_a_bare_new_does_not_leak();
    test_new_zeroes_the_payload();
    test_a_heap_pointer_may_be_returned();
    test_two_objects_are_distinct();
    test_method_on_a_heap_object();
    test_an_object_can_hold_another();

    printf("All heap tests passed\n");
    return 0;
}
