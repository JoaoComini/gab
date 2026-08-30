#include "support/run.h"

#include <assert.h>

static void test_a_self_referential_struct_declares() {
    assert(test_compiles("struct Node { parent: &Node, child: *Node }\n"));

    assert(test_compiles("struct Node { child: *Node }\n"));

    assert(!test_compiles("struct Node { self: Node }\n"));
}

static void test_conversion_is_owned_to_ref_only() {
    assert(
        test_compiles("struct Node { n: int }\n"
                      "func f(): int { let o: *Node = box Node { n: 0 }; let b: &Node = o; return 0; }\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "func f(): int {\n"
                          "    let owner: *Node = box Node { n: 0 };\n"
                          "    let b: &Node = owner;\n"
                          "    let o: *Node = b;\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_a_top_level_variable_may_not_own() {
    assert(!test_compiles("struct Node { n: int }\nlet n: *Node = box Node { n: 0 };\n"));
    assert(!test_compiles("let s: String = \"ab\".to_owned();\n"));

    assert(test_compiles("let s: &str = \"hi\";\n"));
}

static void test_ref_and_box_nest_in_any_order() {
    assert(test_compiles("struct Node { n: int }\nfunc f(): int { let a: &*Node; return 0; }\n"));
    assert(test_compiles("struct Node { n: int }\nfunc f(): int { let b: *&Node; return 0; }\n"));
    assert(test_compiles("struct Node { n: int }\nfunc f(): int { let c: &*&Node; return 0; }\n"));
}

static void test_box_allocates_the_value_it_is_given() {
    assert(test_compiles("struct Node { n: int }\n"
                         "func f(): int { let o: **Node = box (box Node { n: 0 }); return 0; }\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "func f(): int { let n = Node { n: 0 }; let b: &Node = n;\n"
                          "                let o: *&Node = box b; return 0; }\n"));
}

static void test_freeing_an_owning_pointer_frees_beneath_it() {
    assert(test_run_int("struct Node { n: int }\n"
                        "func f(): int {\n"
                        "    let o: **Node = box (box Node { n: 0 });\n"
                        "    *o = box Node { n: 0 };\n"
                        "    o.n = 4;\n"
                        "    return o.n;\n"
                        "}\n"
                        "let r: int = f();") == 4);
}

static void test_lending_stops_at_the_first_level_that_matches() {
    assert(test_run_int("struct Node { n: int }\n"
                        "func object(b: &Node): int { return b.n; }\n"
                        "func slot(s: &*Node): int { return s.n + 3; }\n"
                        "func f(): int {\n"
                        "    let o: **Node = box (box Node { n: 0 });\n"
                        "    *o = box Node { n: 0 };\n"
                        "    o.n = 4;\n"
                        "    return object(o) * 100 + slot(o);\n"
                        "}\n"
                        "let r: int = f();") == 407);
}

static void test_lending_reaches_through_a_borrow() {
    assert(test_run_int("struct Node { n: int }\n"
                        "func object(b: &Node): int { return b.n; }\n"
                        "func slot(s: &*Node): int { return object(s); }\n"
                        "func f(): int {\n"
                        "    let o: *Node = box Node { n: 0 };\n"
                        "    o.n = 4;\n"
                        "    return slot(o);\n"
                        "}\n"
                        "let r: int = f();") == 4);

    assert(test_run_int("struct Node { n: int }\n"
                        "func fill(b: &Node): int { b.n = 8; return 0; }\n"
                        "func slot(s: &*Node): int { fill(s); return 0; }\n"
                        "func f(): int {\n"
                        "    let o: *Node = box Node { n: 0 };\n"
                        "    slot(o);\n"
                        "    return o.n;\n"
                        "}\n"
                        "let r: int = f();") == 8);
}

static void test_lending_through_a_borrow_keeps_its_lifetime() {
    assert(!test_compiles("struct Node { n: int }\n"
                          "func bad(): &Node {\n"
                          "    let local: *Node = box Node { n: 0 };\n"
                          "    let s: &*Node = local;\n"
                          "    return s;\n"
                          "}\n"));

    assert(test_compiles("struct Node { n: int }\n"
                         "func ok(s: &*Node): &Node { return s; }\n"));
}

static void test_a_value_initializes_a_ref_binding() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let owned = Box { n: 4 };\n"
                        "    let borrowed: &Box = owned;\n"
                        "    return borrowed.n;\n"
                        "}\n"
                        "let r: int = main();") == 4);
}

static void test_a_borrowed_binding_writes_through() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func main(): int {\n"
                        "    let owned = Box { n: 1 };\n"
                        "    let borrowed: &Box = owned;\n"
                        "    borrowed.n = 9;\n"
                        "    return owned.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

static void test_a_temporary_cannot_be_borrowed() {
    assert(!test_compiles("func f(): int { let p: &int = 1; return *p; }\n"));

    assert(!test_compiles("struct Box { n: int }\n"
                          "func make(): Box { let b = Box { n: 0 }; return b; }\n"
                          "func peek(b: &Box): int { return b.n; }\n"
                          "func main(): int { return peek(make()); }\n"));
}

static void test_a_field_reaches_through_every_pointer_level() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(s: &*Box): int { return s.n; }\n"
                        "func main(): int {\n"
                        "    let o: *Box = box Box { n: 0 };\n"
                        "    o.n = 6;\n"
                        "    return peek(o);\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

static void test_a_method_reaches_through_every_pointer_level() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func Box::bump(b: &Box): int { b.n = b.n + 1; return b.n; }\n"
                        "func poke(s: &*Box): int { return s.bump(); }\n"
                        "func main(): int {\n"
                        "    let o: *Box = box Box { n: 0 };\n"
                        "    o.n = 11;\n"
                        "    return poke(o) * 100 + o.n;\n"
                        "}\n"
                        "let r: int = main();") == 1212);
}

static void test_an_out_parameter_repoints_the_callers_slot() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func replace(s: &*Box): int { *s = box Box { n: 0 }; (*s).n = 9; return 0; }\n"
                        "func main(): int {\n"
                        "    let o: *Box = box Box { n: 0 };\n"
                        "    o.n = 1;\n"
                        "    replace(o);\n"
                        "    return o.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

static void test_an_out_parameter_leaves_one_owner() {
    TestProgram program = test_compile("struct Box { n: int }\n"
                                       "func replace(s: &*Box): int { *s = box Box { n: 0 }; return 0; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_RELEASE) == 1);

    test_program_free(&program);
}

static void test_a_ref_field_reads_and_writes() {
    assert(test_run_int("struct Node { n: int, parent: &Leaf }\n"
                        "struct Leaf { n: int }\n"
                        "func main(): int {\n"
                        "    let a = Leaf { n: 7 };\n"
                        "    let b: *Node = box Node { n: 0, parent: a };\n"
                        "    return b.parent.n;\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

static void test_a_ref_passes_to_a_ref_parameter() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(b: &Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = box Box { n: 0 };\n"
                        "    owner.n = 8;\n"
                        "    let borrowed: &Box = owner;\n"
                        "    return peek(borrowed);\n"
                        "}\n"
                        "let r: int = main();") == 8);
}

static void test_an_owned_pointer_passes_to_a_ref_parameter() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func peek(b: &Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = box Box { n: 0 };\n"
                        "    owner.n = 3;\n"
                        "    return peek(owner);\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

static void test_a_call_returning_a_ref_is_not_freed() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func borrow(b: &Box): &Box { return b; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = box Box { n: 0 };\n"
                        "    owner.n = 2;\n"
                        "    let got: &Box = borrow(owner);\n"
                        "    return got.n + owner.n;\n"
                        "}\n"
                        "let r: int = main();") == 4);
}

static void test_a_ref_to_a_local_cannot_be_returned() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func bad(): &Box {\n"
                          "    let local = Box { n: 0 };\n"
                          "    return local;\n"
                          "}\n"));
}

static void test_a_ref_returned_from_a_call_cannot_outlive_its_argument() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func borrow(b: &Box): &Box { return b; }\n"
                          "func main(): int {\n"
                          "    let escaped: &Box;\n"
                          "    { let inner = Box { n: 0 }; escaped = borrow(inner); }\n"
                          "    return escaped.n;\n"
                          "}\n"));
}

static void test_a_ref_declared_from_a_call_carries_the_argument_lifetime() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func borrow(b: &Box): &Box { return b; }\n"
                          "func leak(): &Box {\n"
                          "    let local = Box { n: 0 };\n"
                          "    let got: &Box = borrow(local);\n"
                          "    return got;\n"
                          "}\n"));
}

static void test_a_ref_borrowed_from_a_heap_object_is_accepted() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func borrow(b: &Box): &Box { return b; }\n"
                        "func main(): int {\n"
                        "    let owner: *Box = box Box { n: 0 };\n"
                        "    owner.n = 5;\n"
                        "    let got: &Box = borrow(owner);\n"
                        "    return got.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

static void test_an_owned_return_does_not_inherit_argument_lifetimes() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func make(seed: &Box): *Box {\n"
                        "    let fresh: *Box = box Box { n: 0 };\n"
                        "    fresh.n = seed.n + 1;\n"
                        "    return fresh;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let out: *Box = box Box { n: 0 };\n"
                        "    { let tmp: *Box = box Box { n: 0 }; tmp.n = 1; out = make(tmp); }\n"
                        "    return out.n;\n"
                        "}\n"
                        "let r: int = main();") == 2);
}

static void test_an_owned_return_is_still_allowed() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func make(): *Box {\n"
                        "    let b: *Box = box Box { n: 0 };\n"
                        "    b.n = 4;\n"
                        "    return b;\n"
                        "}\n"
                        "func main(): int { let b: *Box = make(); return b.n; }\n"
                        "let r: int = main();") == 4);
}

static void test_an_owning_pointer_does_not_reach_a_double_borrow() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func replace(slot: &&Box): int { return 0; }\n"
                          "func main(): int {\n"
                          "    let o: *Box = box Box { n: 0 };\n"
                          "    return replace(o);\n"
                          "}\n"));
}

static void test_a_borrow_of_a_borrow_is_allowed() {
    assert(test_run_int("func f(): int {\n"
                        "    let x: int = 5;\n"
                        "    let p: &int = x;\n"
                        "    let q: &&int = p;\n"
                        "    **q = 11;\n"
                        "    return x;\n"
                        "}\n"
                        "let r: int = f();") == 11);
}

static void test_a_ref_parameter_is_an_out_parameter_for_values() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func fill(b: &Box): int { b.n = 42; return 0; }\n"
                        "func main(): int {\n"
                        "    let o: *Box = box Box { n: 0 };\n"
                        "    fill(o);\n"
                        "    return o.n;\n"
                        "}\n"
                        "let r: int = main();") == 42);
}

static void test_an_owned_temporary_in_an_assignment_does_not_leak() {
    TestProgram program =
        test_compile("struct Node { v: int }\n"
                     "func f(): int { let x: int = 0; x = (box Node { v: 0 }).v; return x; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_find_opcode(chunk, OP_RELEASE) > test_find_opcode(chunk, OP_BOX));
    assert(test_find_opcode(chunk, OP_RELEASE) < test_find_opcode(chunk, OP_RETURN));

    assert(test_count_opcode(chunk, OP_RELEASE) == 1);

    test_program_free(&program);
}

static void test_a_field_read_from_an_owned_temporary_does_not_leak() {
    TestProgram program = test_compile("struct Node { v: int }\n"
                                       "func f(): int { return (box Node { v: 0 }).v; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_find_opcode(chunk, OP_RELEASE) > 0);
    assert(test_find_opcode(chunk, OP_RELEASE) < test_find_opcode(chunk, OP_RETURN));

    test_program_free(&program);
}

static void test_a_branch_join_takes_the_shorter_lived_borrow() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let heap: *Box = box Box { n: 0 };\n"
                          "    let out: &Box = heap;\n"
                          "    {\n"
                          "        let inner = Box { n: 0 };\n"
                          "        let a: &Box = heap;\n"
                          "        if 1 < 2 { a = inner; } else { a = heap; }\n"
                          "        out = a;\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));

    assert(test_compiles("struct Box { n: int }\n"
                         "func main(): int {\n"
                         "    let heap: *Box = box Box { n: 0 };\n"
                         "    let out: &Box = heap;\n"
                         "    {\n"
                         "        let inner = Box { n: 0 };\n"
                         "        let a: &Box = heap;\n"
                         "        if 1 < 2 { a = heap; } else { a = heap; }\n"
                         "        out = a;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n"));
}

static void test_a_borrow_taken_late_in_a_loop_reaches_the_next_iteration() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let heap: *Box = box Box { n: 0 };\n"
                          "    let out: &Box = heap;\n"
                          "    {\n"
                          "        let inner = Box { n: 0 };\n"
                          "        let a: &Box = heap;\n"
                          "        for let i = 0; i < 2; i = i + 1 {\n"
                          "            out = a;\n"
                          "            a = inner;\n"
                          "        }\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_reassigning_a_borrow_replaces_what_it_names() {
    assert(test_compiles("struct Box { n: int }\n"
                         "func main(): int {\n"
                         "    let heap: *Box = box Box { n: 0 };\n"
                         "    let out: &Box = heap;\n"
                         "    {\n"
                         "        let inner = Box { n: 0 };\n"
                         "        let a: &Box = inner;\n"
                         "        a = heap;\n"
                         "        out = a;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n"));

    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let heap: *Box = box Box { n: 0 };\n"
                          "    let out: &Box = heap;\n"
                          "    {\n"
                          "        let inner = Box { n: 0 };\n"
                          "        let a: &Box = inner;\n"
                          "        out = a;\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_an_arm_that_returns_does_not_reach_the_join() {
    assert(test_compiles("struct Box { n: int }\n"
                         "func main(): int {\n"
                         "    let heap: *Box = box Box { n: 0 };\n"
                         "    let out: &Box = heap;\n"
                         "    {\n"
                         "        let inner = Box { n: 0 };\n"
                         "        let a: &Box = heap;\n"
                         "        if 1 < 2 { a = inner; return 0; } else { a = heap; }\n"
                         "        out = a;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n"));

    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let heap: *Box = box Box { n: 0 };\n"
                          "    let out: &Box = heap;\n"
                          "    {\n"
                          "        let inner = Box { n: 0 };\n"
                          "        let a: &Box = heap;\n"
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
    test_box_allocates_the_value_it_is_given();
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

    printf("All ownership tests passed\n");
    return 0;
}
