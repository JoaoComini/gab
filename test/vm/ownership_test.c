// Which positions may own and which must borrow, and what a borrow may do once
// it exists. '*T' marks a slot that can free what it holds — a 'let', a struct
// field, a return type — and every other position takes 'ref T'.

#include "support/run.h"

#include <assert.h>

// A struct may name its own type through a field, which is what lets a child
// know its parent without owning it.
static void test_a_self_referential_struct_declares() {
    assert(test_compiles("struct Node { parent: ref Node, child: box Node }\n"));

    // A pointer to self is not containment, borrowed or owned.
    assert(test_compiles("struct Node { child: box Node }\n"));

    // Containing itself by value still is.
    assert(!test_compiles("struct Node { self: Node }\n"));
}

// An owned pointer may be stored where a borrow is expected: giving something
// up to be named costs nothing. The reverse would hand out ownership nobody
// granted, so it is refused.
static void test_conversion_is_owned_to_ref_only() {
    assert(test_compiles("struct Node { n: int }\n"
                         "func f(): int { let o: box Node = new Node; let b: ref Node = o; return 0; }\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "func f(): int {\n"
                          "    let owner: box Node = new Node;\n"
                          "    let b: ref Node = owner;\n"
                          "    let o: box Node = b;\n"
                          "    return 0;\n"
                          "}\n"));
}

// 'ref' stands in place of the '*', so combining the two has nothing to mean:
// the flag qualifies one pointer, and there would be two.
static void test_ref_does_not_combine_with_a_star() {
    assert(!test_compiles("struct Node { n: int }\nlet x: ref *Node;\n"));
}

// A 'ref' field is still a field: it reads and writes like any other pointer,
// since the address is identical and only ownership differs.
static void test_a_ref_field_reads_and_writes() {
    assert(test_run_int("struct Node { n: int, parent: ref Node }\n"
                        "func main(): int {\n"
                        "    let a: box Node = new Node;\n"
                        "    let b: box Node = new Node;\n"
                        "    a.n = 7;\n"
                        "    b.parent = a;\n"
                        "    return b.parent.n;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

// A 'ref T' argument where the parameter declares one: a borrow passed on as a
// borrow, which is what most behaviour code does.
static void test_a_ref_passes_to_a_ref_parameter() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: box Box = new Box;\n"
                        "    owner.n = 8;\n"
                        "    let borrowed: ref Box = owner;\n"
                        "    return peek(borrowed);\n"
                        "}\n"
                        "let r: int = main();") == 8);
}

// An owned '*T' where a 'ref T' parameter is declared: giving something up to
// be named costs nothing, and the caller goes on owning it.
static void test_an_owned_pointer_passes_to_a_ref_parameter() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: box Box = new Box;\n"
                        "    owner.n = 3;\n"
                        "    return peek(owner);\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

// A call returning 'ref T' hands back a borrow, so nothing at the call site
// frees it and the object stays its owner's.
static void test_a_call_returning_a_ref_is_not_freed() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func borrow(b: ref Box): ref Box { return b; }\n"
                        "func main(): int {\n"
                        "    let owner: box Box = new Box;\n"
                        "    owner.n = 2;\n"
                        "    let got: ref Box = borrow(owner);\n"
                        "    return got.n + owner.n;\n"
                        "}\n"
                        "let r: int = main();") == 4);
}

// Returning a borrow is safe exactly when it names something that outlives the
// call, which is C++'s rule for returning a reference: a member or something
// the caller passed in, never a local.
//
// A parameter is the safe case, covered above. This is the unsafe one, and the
// lifetime check already refuses it — a 'ref' to a local would name a frame
// slot the caller reuses the moment it returns.
static void test_a_ref_to_a_local_cannot_be_returned() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func bad(): ref Box {\n"
                          "    let local: Box;\n"
                          "    return ref local;\n"
                          "}\n"));
}

// A borrow handed back by a call is only as long-lived as what it was made
// from. Nothing says which argument it borrows without a per-function summary,
// so the result is treated as borrowing from the shortest-lived of them --
// which is what makes this checkable at all.
//
// Here the borrow outlives its pointee: 'inner' dies at the brace and 'escaped'
// is declared outside it.
static void test_a_ref_returned_from_a_call_cannot_outlive_its_argument() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func borrow(b: ref Box): ref Box { return b; }\n"
                          "func main(): int {\n"
                          "    let escaped: ref Box;\n"
                          "    { let inner: Box; escaped = borrow(ref inner); }\n"
                          "    return escaped.n;\n"
                          "}\n"));
}

// The same shape at a declaration rather than an assignment, since both record
// what a variable now points at.
static void test_a_ref_declared_from_a_call_carries_the_argument_lifetime() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func borrow(b: ref Box): ref Box { return b; }\n"
                          "func leak(): ref Box {\n"
                          "    let local: Box;\n"
                          "    let got: ref Box = borrow(ref local);\n"
                          "    return got;\n"
                          "}\n"));
}

// A borrow of something that outlives the call is fine, which is the useful
// half: a heap object outlives every frame, so the result may go anywhere.
static void test_a_ref_borrowed_from_a_heap_object_is_accepted() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func borrow(b: ref Box): ref Box { return b; }\n"
                        "func main(): int {\n"
                        "    let owner: box Box = new Box;\n"
                        "    owner.n = 5;\n"
                        "    let got: ref Box = borrow(owner);\n"
                        "    return got.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

// An owned '*T' returned by a call is a heap object whatever it was made from,
// so it outlives every frame and the argument's lifetime does not follow it.
static void test_an_owned_return_does_not_inherit_argument_lifetimes() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func make(seed: ref Box): box Box {\n"
                        "    let fresh: box Box = new Box;\n"
                        "    fresh.n = seed.n + 1;\n"
                        "    return fresh;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let out: box Box = new Box;\n"
                        "    { let tmp: box Box = new Box; tmp.n = 1; out = make(tmp); }\n"
                        "    return out.n;\n"
                        "}\n"
                        "let r: int = main();") == 2);
}

// An owned return is how a function hands ownership out.
static void test_an_owned_return_is_still_allowed() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func make(): box Box {\n"
                        "    let b: box Box = new Box;\n"
                        "    b.n = 4;\n"
                        "    return b;\n"
                        "}\n"
                        "func main(): int { let b: box Box = make(); return b.n; }\n"
                        "let r: int = main();") == 4);
}

// '&o' where 'o' owns yields a borrow of an owning pointer -- 'ref *Box' -- and
// that type cannot be written: 'ref' does not combine with '*'. Producing a
// value nothing can name is worse than refusing it, so this is refused.
//
// The type is what an out-parameter would need: a borrow of the caller's
// variable rather than of the object, so the callee could repoint it. That is
// more than syntax, since assigning through one would free the caller's old
// object from inside the callee -- an owning slot changing owner mid-call.
// Returning ownership says the same thing with the transfer visible.
static void test_taking_the_address_of_an_owning_pointer_is_refused() {
    // Nothing constrains the type here, so only the address-of itself can
    // refuse it. Written as a bare statement for that reason: given a
    // declaration to mismatch against, this would fail either way and the test
    // would not say which rule caught it.
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let o: box Box = new Box;\n"
                          "    ref o;\n"
                          "    return 0;\n"
                          "}\n"));

    // And as an argument, which is where an out-parameter would be attempted.
    assert(!test_compiles("struct Box { n: int }\n"
                          "func replace(slot: ref ref Box): int { return 0; }\n"
                          "func main(): int {\n"
                          "    let o: box Box = new Box;\n"
                          "    return replace(ref o);\n"
                          "}\n"));
}

// A borrow of a borrow is still fine: 'ref ref T' is writable, and '&' applied
// to a 'ref T' produces exactly it.
static void test_taking_the_address_of_a_borrow_is_allowed() {
    assert(test_run_int("func f(): int {\n"
                        "    let x: int = 5;\n"
                        "    let p: ref int = ref x;\n"
                        "    let q: ref ref int = ref p;\n"
                        "    **q = 11;\n"
                        "    return x;\n"
                        "}\n"
                        "let r: int = f();") == 11);
}

// Writing through a borrow is the out-parameter that does work: the callee
// fills in a struct the caller owns, which is what 'ref' is for.
static void test_a_ref_parameter_is_an_out_parameter_for_values() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func fill(b: ref Box): int { b.n = 42; return 0; }\n"
                        "func main(): int {\n"
                        "    let o: box Box = new Box;\n"
                        "    fill(o);\n"
                        "    return o.n;\n"
                        "}\n"
                        "let r: int = main();") == 42);
}

// The same where the statement is not a return: the release belongs to the
// statement that produced the temporary, whichever statement that is.
static void test_an_owned_temporary_in_an_assignment_does_not_leak() {
    TestProgram program = test_compile("struct Node { v: int }\n"
                                       "func f(): int { let x: int = 0; x = (new Node).v; return x; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_find_opcode(chunk, OP_RELEASE) > test_find_opcode(chunk, OP_NEW));
    assert(test_find_opcode(chunk, OP_RELEASE) < test_find_opcode(chunk, OP_RETURN));

    // Released once: the statement that produced it takes it off the list, so the
    // return has nothing left to free.
    assert(test_count_opcode(chunk, OP_RELEASE) == 1);

    test_program_free(&program);
}

// An owned value no slot names is still freed: a field read reaches into the
// object for the length of the expression, and nothing binds it afterwards.
static void test_a_field_read_from_an_owned_temporary_does_not_leak() {
    TestProgram program = test_compile("struct Node { v: int }\n"
                                       "func f(): int { return (new Node).v; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    // Before the return, or it never runs.
    assert(test_find_opcode(chunk, OP_RELEASE) > 0);
    assert(test_find_opcode(chunk, OP_RELEASE) < test_find_opcode(chunk, OP_RETURN));

    test_program_free(&program);
}

// Where two branches rejoin, a borrow is as short-lived as the shorter-lived
// of what the arms left in it: it is safe at a use only if it is safe on every
// path reaching that use.
static void test_a_branch_join_takes_the_shorter_lived_borrow() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let heap: box Box = new Box;\n"
                          "    let out: ref Box = heap;\n"
                          "    {\n"
                          "        let inner: Box;\n"
                          "        let a: ref Box = heap;\n"
                          "        if 1 < 2 { a = ref inner; } else { a = heap; }\n"
                          "        out = a;\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));

    // Neither arm leaves a borrow of the block's own local, so the same copy
    // out of the block is accepted.
    assert(test_compiles("struct Box { n: int }\n"
                         "func main(): int {\n"
                         "    let heap: box Box = new Box;\n"
                         "    let out: ref Box = heap;\n"
                         "    {\n"
                         "        let inner: Box;\n"
                         "        let a: ref Box = heap;\n"
                         "        if 1 < 2 { a = heap; } else { a = heap; }\n"
                         "        out = a;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n"));
}

// A loop body's back-edge carries what the tail of one iteration left to the
// head of the next, so a borrow taken late is checked against the code that
// reads it early.
static void test_a_borrow_taken_late_in_a_loop_reaches_the_next_iteration() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let heap: box Box = new Box;\n"
                          "    let out: ref Box = heap;\n"
                          "    {\n"
                          "        let inner: Box;\n"
                          "        let a: ref Box = heap;\n"
                          "        for let i = 0; i < 2; i = i + 1 {\n"
                          "            out = a;\n"
                          "            a = ref inner;\n"
                          "        }\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

// A borrow's depth is what the last assignment left, not the deepest the slot
// ever held: reassigning it something longer-lived makes it usable again.
static void test_reassigning_a_borrow_replaces_what_it_names() {
    assert(test_compiles("struct Box { n: int }\n"
                         "func main(): int {\n"
                         "    let heap: box Box = new Box;\n"
                         "    let out: ref Box = heap;\n"
                         "    {\n"
                         "        let inner: Box;\n"
                         "        let a: ref Box = ref inner;\n"
                         "        a = heap;\n"
                         "        out = a;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n"));

    // Without that reassignment the copy names the block's local and is
    // refused.
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let heap: box Box = new Box;\n"
                          "    let out: ref Box = heap;\n"
                          "    {\n"
                          "        let inner: Box;\n"
                          "        let a: ref Box = ref inner;\n"
                          "        out = a;\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

// An arm that cannot fall through is not a way of arriving after the 'if', so
// what it left in a slot does not constrain the code that follows.
static void test_an_arm_that_returns_does_not_reach_the_join() {
    assert(test_compiles("struct Box { n: int }\n"
                         "func main(): int {\n"
                         "    let heap: box Box = new Box;\n"
                         "    let out: ref Box = heap;\n"
                         "    {\n"
                         "        let inner: Box;\n"
                         "        let a: ref Box = heap;\n"
                         "        if 1 < 2 { a = ref inner; return 0; } else { a = heap; }\n"
                         "        out = a;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n"));

    // The same arm falling through does reach it, and is refused.
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let heap: box Box = new Box;\n"
                          "    let out: ref Box = heap;\n"
                          "    {\n"
                          "        let inner: Box;\n"
                          "        let a: ref Box = heap;\n"
                          "        if 1 < 2 { a = ref inner; } else { a = heap; }\n"
                          "        out = a;\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

int main(void) {
    test_an_arm_that_returns_does_not_reach_the_join();
    test_a_branch_join_takes_the_shorter_lived_borrow();
    test_a_borrow_taken_late_in_a_loop_reaches_the_next_iteration();
    test_reassigning_a_borrow_replaces_what_it_names();
    test_taking_the_address_of_an_owning_pointer_is_refused();
    test_taking_the_address_of_a_borrow_is_allowed();
    test_a_ref_parameter_is_an_out_parameter_for_values();
    test_an_owned_return_is_still_allowed();
    test_a_ref_to_a_local_cannot_be_returned();
    test_a_ref_returned_from_a_call_cannot_outlive_its_argument();
    test_a_ref_declared_from_a_call_carries_the_argument_lifetime();
    test_a_ref_borrowed_from_a_heap_object_is_accepted();
    test_an_owned_return_does_not_inherit_argument_lifetimes();
    test_a_ref_passes_to_a_ref_parameter();
    test_an_owned_pointer_passes_to_a_ref_parameter();
    test_a_call_returning_a_ref_is_not_freed();
    test_a_self_referential_struct_declares();
    test_a_field_read_from_an_owned_temporary_does_not_leak();
    test_an_owned_temporary_in_an_assignment_does_not_leak();
    test_conversion_is_owned_to_ref_only();
    test_ref_does_not_combine_with_a_star();
    test_a_ref_field_reads_and_writes();

    printf("All ref tests passed\n");
    return 0;
}
