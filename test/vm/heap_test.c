#include "compile.h"
#include "diagnostics.h"
#include "support/run.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <assert.h>

#include <stdio.h>

// 'new T' yields a 'box T' into the heap: writing through it and reading back is
// what proves the allocation is real and the pointer addresses the payload.
static void test_new_allocates_a_usable_object() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func main(): int {\n"
                        "    let p: box Player = new Player;\n"
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
                        "    let p: box Player = new Player;\n"
                        "    return p.health + p.mana;\n"
                        "}\n"
                        "let r: int = main();") == 0);
}

// A heap pointer outlives every frame, so returning one is allowed where
// returning '&local' is not.
static void test_a_heap_pointer_may_be_returned() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func make(): box Box {\n"
                        "    let b: box Box = new Box;\n"
                        "    b.n = 7;\n"
                        "    return b;\n"
                        "}\n"
                        "func main(): int { let b: box Box = make(); return b.n; }\n"
                        "let r: int = main();") == 7);
}

// Two allocations of the same type share one entry in the VM's type list, and
// two objects that do not alias.
static void test_two_objects_are_distinct() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: box Box = new Box;\n"
                        "    let b: box Box = new Box;\n"
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
                     "    let p: box Player = new Player;\n"
                     "    p.health = 50;\n"
                     "    return p.hurt(8);\n"
                     "}\n"
                     "let r: int = main();") == 42);
}

// A heap object holding a pointer to another: the field is a 'box T' like any
// other, which is what release will later walk.
static void test_an_object_can_hold_another() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { child: box Inner }\n"
                        "func main(): int {\n"
                        "    let o: box Outer = new Outer;\n"
                        "    o.child = new Inner;\n"
                        "    o.child.n = 9;\n"
                        "    return o.child.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// Two names for one object. The second borrows, which is what 'ref' spells:
// the first goes on owning, so nothing frees the object twice.
static void test_an_alias_is_borrowed_not_owned() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let a: box Box = new Box;\n"
                        "    a.n = 5;\n"
                        "    let b: ref Box = a;\n"
                        "    return b.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);

    // Naming it a second time as an owner would make two slots free it, so it
    // takes a 'move' -- and then the first name is dead.
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: box Box = new Box;\n"
                          "    let b: box Box = a;\n"
                          "    return b.n;\n"
                          "}\n"));
}

// Freed where its block closes, not where the frame pops: destruction is
// deterministic at the brace rather than whenever a collector runs.
static void test_released_at_the_end_of_its_block() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let t: int = 0;\n"
                        "    { let a: box Box = new Box; a.n = 3; t = a.n; }\n"
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
                        "    { let a: box Box = new Box; a.n = 1; }\n"
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

// A failure unwinds past every free codegen emitted, so the frames drop what
// they own on the way out.
static void test_an_abnormal_unwind_frees_what_it_held() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype script;
    assert(compile_unit(vm,
                        "module test;\n"
                        "struct Node { n: int }\n"
                        "func deep(n: int): int { return deep(n + 1); }\n"
                        "func main(): int { let p: box Node = new Node; return deep(0); }\n"
                        "let r: int = main();",
                        &script, &diagnostics));

    diagnostics_free(&diagnostics);

    assert(interp_run_top_level(vm, &script) == VM_RUN_ERR_CALL_DEPTH);

    func_proto_free(&script);
    vm_free(vm);
}

// A strong field owns what it names, so it may only be given something nothing
// else owns. 'i' is owned by its own slot, and storing it would leave the object
// with two owners — the field and the slot — each releasing it independently.
static void test_storing_a_borrowed_reference_into_a_field_is_refused() {
    assert(!test_codegens("struct Inner { n: int }\n"
                          "struct Outer { child: box Inner }\n"
                          "func main(): int {\n"
                          "    let o: box Outer = new Outer;\n"
                          "    let i: box Inner = new Inner;\n"
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
                          "struct Outer { child: box Inner }\n"
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
                        "struct Outer { child: box Inner }\n"
                        "func make(): box Inner {\n"
                        "    let i: box Inner = new Inner;\n"
                        "    i.n = 6;\n"
                        "    return i;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let o: box Outer = new Outer;\n"
                        "    o.child = make();\n"
                        "    return o.child.n;\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

// An owned value passed straight into a call belongs to nobody once the call
// returns: a parameter borrows, so the callee does not free it, and the argument
// was never bound to a slot that would. The call site frees it.
//
// Correctness is that it runs clean under LeakSanitizer; the returned value only
// proves the call happened.
static void test_an_owned_argument_is_freed_by_the_call_site() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func take(b: ref Box): int { return b.n + 1; }\n"
                        "func main(): int { return take(new Box); }\n"
                        "let r: int = main();") == 1);
}

// The receiver is parameter zero, so an owned one arrives the same way and is
// nobody's afterwards either.
static void test_an_owned_receiver_is_freed_by_the_call_site() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func (b: ref Box) get(): int { return b.n + 2; }\n"
                        "func main(): int { return (new Box).get(); }\n"
                        "let r: int = main();") == 2);
}

// A borrowed argument is left alone: its own slot still owns it, and freeing at
// the call site would free it out from under the variable that goes on naming
// it.
static void test_a_borrowed_argument_is_not_freed_by_the_call_site() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func take(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let b: box Box = new Box;\n"
                        "    b.n = 5;\n"
                        "    take(b);\n"
                        "    return b.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

// Overwriting a field that already holds a reference drops the old one, or the
// first object leaks with nothing left naming it.
static void test_overwriting_a_field_releases_the_old_value() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { child: box Inner }\n"
                        "func main(): int {\n"
                        "    let o: box Outer = new Outer;\n"
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
                        "    let p: box Box = new Box;\n"
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
                          "struct Outer { child: box Inner }\n"
                          "func main(): int {\n"
                          "    let o: box Outer = new Outer;\n"
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
                          "func main(): int { let p: box Box = new Box; p.n = 3; p = p; return p.n; }\n"
                          "let r: int = main();"));
}

// Reassigning an owning variable with a fresh object is the legal shape, and it
// frees what the slot held before.
static void test_reassigning_an_owning_variable_frees_the_old_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let p: box Box = new Box;\n"
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
    test_an_owned_argument_is_freed_by_the_call_site();
    test_an_owned_receiver_is_freed_by_the_call_site();
    test_a_borrowed_argument_is_not_freed_by_the_call_site();
    test_an_alias_is_borrowed_not_owned();
    test_released_at_the_end_of_its_block();
    test_a_reused_slot_is_not_released_twice();
    test_a_bare_new_does_not_leak();
    test_an_abnormal_unwind_frees_what_it_held();
    test_new_zeroes_the_payload();
    test_a_heap_pointer_may_be_returned();
    test_two_objects_are_distinct();
    test_method_on_a_heap_object();
    test_an_object_can_hold_another();

    printf("All heap tests passed\n");
    return 0;
}
