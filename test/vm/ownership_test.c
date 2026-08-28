// Which positions may own and which must borrow, and what a borrow may do once
// it exists. 'box T' marks a slot that can free what it holds — a 'let', a struct
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

// Nothing closes over the top level, so a value that owns would be freed by no
// one. Owning belongs where a scope ends.
static void test_a_top_level_variable_may_not_own() {
    assert(!test_compiles("struct Node { n: int }\nlet n: box Node = new Node;\n"));
    assert(!test_compiles("let s: String = \"ab\".to_owned();\n"));

    // A borrow frees nothing, so it is at home there.
    assert(test_compiles("let s: ref str = \"hi\";\n"));
}

// 'ref' and 'box' each qualify the one level they spell, so they nest in either
// order and to any depth.
static void test_ref_and_box_nest_in_any_order() {
    assert(test_compiles("struct Node { n: int }\nfunc f(): int { let a: ref box Node; return 0; }\n"));
    assert(test_compiles("struct Node { n: int }\nfunc f(): int { let b: box ref Node; return 0; }\n"));
    assert(test_compiles("struct Node { n: int }\nfunc f(): int { let c: ref box ref Node; return 0; }\n"));
}

// 'new' allocates anything with a layout to fill, which is a struct or an owning
// pointer. A borrow is neither: a heap slot holding one would outlive what it
// borrows with nothing tracking that.
static void test_new_allocates_a_struct_or_an_owning_pointer() {
    assert(test_compiles("struct Node { n: int }\n"
                         "func f(): int { let o: box box Node = new box Node; return 0; }\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "func f(): int { let o: box ref Node = new ref Node; return 0; }\n"));

    assert(!test_compiles("func f(): int { let o: box int = new int; return 0; }\n"));
}

// Freeing a heap slot that owns a pointer frees what the pointer names: it has
// no fields to walk, so that one pointer is the whole of what it owns.
static void test_freeing_an_owning_pointer_frees_beneath_it() {
    assert(test_run_int("struct Node { n: int }\n"
                        "func f(): int {\n"
                        "    let o: box box Node = new box Node;\n"
                        "    *o = new Node;\n"
                        "    o.n = 4;\n"
                        "    return o.n;\n"
                        "}\n"
                        "let r: int = f();") == 4);
}

// Lending walks the pointer levels until one matches what the destination wants,
// and stops there. The same argument therefore reaches a different depth for
// each declaration: 'ref Node' takes the object, 'ref box Node' the slot.
static void test_lending_stops_at_the_first_level_that_matches() {
    assert(test_run_int("struct Node { n: int }\n"
                        "func object(b: ref Node): int { return b.n; }\n"
                        "func slot(s: ref box Node): int { return s.n + 3; }\n"
                        "func f(): int {\n"
                        "    let o: box box Node = new box Node;\n"
                        "    *o = new Node;\n"
                        "    o.n = 4;\n"
                        "    return object(o) * 100 + slot(o);\n"
                        "}\n"
                        "let r: int = f();") == 407);
}

// Lending walks a borrow like any other level: it confers no ownership, so
// reaching through one produces another borrow rather than giving anything away.
static void test_lending_reaches_through_a_borrow() {
    assert(test_run_int("struct Node { n: int }\n"
                        "func object(b: ref Node): int { return b.n; }\n"
                        "func slot(s: ref box Node): int { return object(s); }\n"
                        "func f(): int {\n"
                        "    let o: box Node = new Node;\n"
                        "    o.n = 4;\n"
                        "    return slot(o);\n"
                        "}\n"
                        "let r: int = f();") == 4);

    // And names the one object, so a write through the lent borrow is visible
    // to whoever owns it.
    assert(test_run_int("struct Node { n: int }\n"
                        "func fill(b: ref Node): int { b.n = 8; return 0; }\n"
                        "func slot(s: ref box Node): int { fill(s); return 0; }\n"
                        "func f(): int {\n"
                        "    let o: box Node = new Node;\n"
                        "    slot(o);\n"
                        "    return o.n;\n"
                        "}\n"
                        "let r: int = f();") == 8);
}

// What a borrow reaches is no longer-lived than the borrow itself, so lending
// through one cannot launder a frame-bound inner into something returnable.
static void test_lending_through_a_borrow_keeps_its_lifetime() {
    assert(!test_compiles("struct Node { n: int }\n"
                          "func bad(): ref Node {\n"
                          "    let local: box Node = new Node;\n"
                          "    let s: ref box Node = local;\n"
                          "    return s;\n"
                          "}\n"));

    // A 'ref box T' parameter names the caller's slot, which outlives the call,
    // so lending out of one is the safe half of the same rule.
    assert(test_compiles("struct Node { n: int }\n"
                         "func ok(s: ref box Node): ref Node { return s; }\n"));
}

// A borrow is never spelled, so a declaration asking for one takes the address
// of whatever value it is given.
static void test_a_value_initializes_a_ref_binding() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let owned: Box;\n"
                        "    owned.n = 4;\n"
                        "    let borrowed: ref Box = owned;\n"
                        "    return borrowed.n;\n"
                        "}\n"
                        "let r: int = main();") == 4);
}

// Writing through the borrow reaches the value it was made from, which is what
// distinguishes a borrow from the copy a 'Box' destination would have made.
static void test_a_borrowed_binding_writes_through() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let owned: Box;\n"
                        "    owned.n = 1;\n"
                        "    let borrowed: ref Box = owned;\n"
                        "    borrowed.n = 9;\n"
                        "    return owned.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// A temporary has no home in memory, so there is nothing for the borrow to name
// and the destination cannot be filled.
static void test_a_temporary_cannot_be_borrowed() {
    assert(!test_compiles("func f(): int { let p: ref int = 1; return *p; }\n"));

    assert(!test_compiles("struct Box { n: int }\n"
                          "func make(): Box { let b: Box; return b; }\n"
                          "func peek(b: ref Box): int { return b.n; }\n"
                          "func main(): int { return peek(make()); }\n"));
}

// A 'ref box T' borrows the slot rather than the object, so reaching the object
// is two hops. Field access makes them: it reaches through every pointer level
// until it finds the struct, so the levels do not have to be spelled.
static void test_a_field_reaches_through_every_pointer_level() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(s: ref box Box): int { return s.n; }\n"
                        "func main(): int {\n"
                        "    let o: box Box = new Box;\n"
                        "    o.n = 6;\n"
                        "    return peek(o);\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

// A method call reaches the same way, so a receiver several levels out still
// finds the method and still names the one object.
static void test_a_method_reaches_through_every_pointer_level() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func Box::bump(b: ref Box): int { b.n = b.n + 1; return b.n; }\n"
                        "func poke(s: ref box Box): int { return s.bump(); }\n"
                        "func main(): int {\n"
                        "    let o: box Box = new Box;\n"
                        "    o.n = 11;\n"
                        "    return poke(o) * 100 + o.n;\n"
                        "}\n"
                        "let r: int = main();") == 1212);
}

// An out-parameter: the callee repoints the caller's owning slot, and the caller
// reads the object it was given rather than the one it made. The object replaced
// is freed at the store, since nothing names it once the slot points elsewhere.
static void test_an_out_parameter_repoints_the_callers_slot() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func replace(s: ref box Box): int { *s = new Box; (*s).n = 9; return 0; }\n"
                        "func main(): int {\n"
                        "    let o: box Box = new Box;\n"
                        "    o.n = 1;\n"
                        "    replace(o);\n"
                        "    return o.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// The caller still owns what its slot holds after the call, so exactly one
// release covers the object -- the caller's, reading whatever the slot points
// at by then.
static void test_an_out_parameter_leaves_one_owner() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "func replace(s: ref box Box): int { *s = new Box; return 0; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_RELEASE) == 1);

    test_program_free(&program);
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
                          "    return local;\n"
                          "}\n"));
}

// A borrow handed back by a call is only as long-lived as what it was made
// from. Nothing says which argument it borrows without a per-function summary,
// so the result is treated as borrowing from the shortest-lived of them --
// which is what makes this checkable at all.
//
// Here the borrow outlives its inner: 'inner' dies at the brace and 'escaped'
// is declared outside it.
static void test_a_ref_returned_from_a_call_cannot_outlive_its_argument() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func borrow(b: ref Box): ref Box { return b; }\n"
                          "func main(): int {\n"
                          "    let escaped: ref Box;\n"
                          "    { let inner: Box; escaped = borrow(inner); }\n"
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
                          "    let got: ref Box = borrow(local);\n"
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

// Lending reaches the object, never the slot holding it: a 'box T' fills a
// 'ref T' and stops there.
//
// 'ref box T' is what an out-parameter would need: a borrow of the caller's
// variable rather than of the object, so the callee could repoint it. That is
// more than syntax, since assigning through one would free the caller's old
// object from inside the callee -- an owning slot changing owner mid-call.
// Returning ownership says the same thing with the transfer visible.
static void test_an_owning_pointer_does_not_reach_a_double_borrow() {
    // An argument position, which is where an out-parameter would be attempted.
    // A 'box Box' lends as 'ref Box', never as the 'ref ref Box' that would let
    // the callee repoint the caller's slot.
    assert(!test_compiles("struct Box { n: int }\n"
                          "func replace(slot: ref ref Box): int { return 0; }\n"
                          "func main(): int {\n"
                          "    let o: box Box = new Box;\n"
                          "    return replace(o);\n"
                          "}\n"));
}

// A borrow of a borrow is still fine: 'ref ref T' is writable, and a 'ref T'
// binding into one produces exactly that.
static void test_a_borrow_of_a_borrow_is_allowed() {
    assert(test_run_int("func f(): int {\n"
                        "    let x: int = 5;\n"
                        "    let p: ref int = x;\n"
                        "    let q: ref ref int = p;\n"
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
                          "        if 1 < 2 { a = inner; } else { a = heap; }\n"
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
                          "            a = inner;\n"
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
                         "        let a: ref Box = inner;\n"
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
                          "        let a: ref Box = inner;\n"
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
                         "        if 1 < 2 { a = inner; return 0; } else { a = heap; }\n"
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
                          "        if 1 < 2 { a = inner; } else { a = heap; }\n"
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
    test_an_owning_pointer_does_not_reach_a_double_borrow();
    test_a_borrow_of_a_borrow_is_allowed();
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
    test_a_top_level_variable_may_not_own();
    test_ref_and_box_nest_in_any_order();
    test_new_allocates_a_struct_or_an_owning_pointer();
    test_freeing_an_owning_pointer_frees_beneath_it();
    test_lending_stops_at_the_first_level_that_matches();
    test_lending_reaches_through_a_borrow();
    test_lending_through_a_borrow_keeps_its_lifetime();
    test_a_value_initializes_a_ref_binding();
    test_a_borrowed_binding_writes_through();
    test_a_temporary_cannot_be_borrowed();
    test_a_field_reaches_through_every_pointer_level();
    test_a_method_reaches_through_every_pointer_level();
    test_an_out_parameter_repoints_the_callers_slot();
    test_an_out_parameter_leaves_one_owner();
    test_a_ref_field_reads_and_writes();

    printf("All ref tests passed\n");
    return 0;
}
