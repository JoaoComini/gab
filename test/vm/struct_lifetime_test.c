#include "support/run.h"

static void test_a_struct_holding_a_borrow_of_a_local_is_not_returned() {
    assert(!test_compiles("struct Node { n: int }\n"
                          "struct View { r: &Node }\n"
                          "func bad(): View {\n"
                          "    let owned = Node { n: 1 };\n"
                          "    return View { r: owned };\n"
                          "}\n"));

    assert(test_compiles("struct Node { n: int }\n"
                         "struct View { r: &Node }\n"
                         "func ok(p: &Node): View { return View { r: p }; }\n"));
}

static void test_a_borrow_nested_deeper_than_one_struct_is_still_seen() {
    assert(!test_compiles("struct Node { n: int }\n"
                          "struct View { r: &Node }\n"
                          "struct Outer { v: View }\n"
                          "func bad(): Outer {\n"
                          "    let owned = Node { n: 1 };\n"
                          "    return Outer { v: View { r: owned } };\n"
                          "}\n"));
}

static void test_a_struct_of_borrows_assigned_outward_names_what_outlives_it() {
    assert(!test_compiles("struct Node { n: int }\n"
                          "struct View { r: &Node }\n"
                          "func bad(): int {\n"
                          "    let v: View;\n"
                          "    if true {\n"
                          "        let inner = Node { n: 1 };\n"
                          "        v = View { r: inner };\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_a_struct_that_owns_its_field_is_free_to_leave() {
    assert(test_run_int("struct Node { n: int }\n"
                        "struct Holder { b: *Node }\n"
                        "func make(): Holder { return Holder { b: box Node { n: 5 } }; }\n"
                        "func main(): int { let h = make(); return h.b.n; }\n"
                        "let r: int = main();") == 5);
}

static void test_a_struct_of_borrows_lives_as_long_as_what_it_names() {
    assert(test_run_int("struct Node { n: int }\n"
                        "struct View { r: &Node }\n"
                        "func read(v: View): int { return v.r.n; }\n"
                        "func main(): int {\n"
                        "    let owned = Node { n: 6 };\n"
                        "    let v = View { r: owned };\n"
                        "    return read(v);\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

static void test_a_returned_struct_of_borrows_names_the_arguments_it_came_from() {
    assert(!test_compiles("struct Node { n: int }\n"
                          "struct View { r: &Node }\n"
                          "func make(p: &Node): View { return View { r: p }; }\n"
                          "func bad(): View {\n"
                          "    let owned = Node { n: 1 };\n"
                          "    return make(owned);\n"
                          "}\n"));

    assert(test_compiles("struct Node { n: int }\n"
                         "struct View { r: &Node }\n"
                         "func make(p: &Node): View { return View { r: p }; }\n"
                         "func ok(p: &Node): View { return make(p); }\n"));
}

static void test_a_returned_struct_that_owns_is_free_of_its_arguments() {
    assert(test_run_int("struct Node { n: int }\n"
                        "struct Holder { b: *Node }\n"
                        "func make(p: &Node): Holder { return Holder { b: box Node { n: p.n } }; }\n"
                        "func main(): int {\n"
                        "    let owned = Node { n: 9 };\n"
                        "    let h = make(owned);\n"
                        "    return h.b.n;\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

static void test_a_returned_struct_names_only_the_arguments_it_reaches() {
    assert(test_compiles("struct Node { n: int }\n"
                         "struct View { r: &Node }\n"
                         "func make(p: &Node, q: &Node): View { return View { r: p }; }\n"
                         "func ok(p: &Node): View { let local = Node { n: 1 }; return make(p, local); }\n"));

    assert(
        !test_compiles("struct Node { n: int }\n"
                       "struct View { r: &Node }\n"
                       "func make(p: &Node, q: &Node): View { return View { r: p }; }\n"
                       "func bad(q: &Node): View { let local = Node { n: 1 }; return make(local, q); }\n"));
}

static void test_a_field_holds_a_borrow_as_long_as_the_struct_it_sits_in() {
    assert(test_compiles("struct Node { n: int }\n"
                         "struct View { r: &Node }\n"
                         "func f(p: &Node): int {\n"
                         "    if true {\n"
                         "        let inner = Node { n: 1 };\n"
                         "        let v = View { r: p };\n"
                         "        v.r = inner;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "struct View { r: &Node }\n"
                          "func f(p: &Node): int {\n"
                          "    let v = View { r: p };\n"
                          "    if true {\n"
                          "        let inner = Node { n: 1 };\n"
                          "        v.r = inner;\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_a_field_reached_through_a_heap_slot_outlives_every_scope() {
    assert(!test_compiles("struct Node { n: int }\n"
                          "struct View { r: &Node }\n"
                          "func f(p: &Node): int {\n"
                          "    if true {\n"
                          "        let inner = Node { n: 1 };\n"
                          "        let h = box View { r: p };\n"
                          "        h.r = inner;\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_a_field_given_a_borrow_narrows_the_struct_that_holds_it() {
    assert(!test_compiles("struct Node { n: int }\n"
                          "struct View { r: &Node }\n"
                          "func f(p: &Node): &Node {\n"
                          "    let v = View { r: p };\n"
                          "    let l = Node { n: 1 };\n"
                          "    v.r = l;\n"
                          "    return v.r;\n"
                          "}\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "struct View { r: &Node }\n"
                          "func f(p: &Node): View {\n"
                          "    let v = View { r: p };\n"
                          "    let l = Node { n: 1 };\n"
                          "    v.r = l;\n"
                          "    return v;\n"
                          "}\n"));

    assert(test_compiles("struct Node { n: int }\n"
                         "struct View { r: &Node }\n"
                         "func f(p: &Node, q: &Node): View {\n"
                         "    let v = View { r: p };\n"
                         "    v.r = q;\n"
                         "    return v;\n"
                         "}\n"));
}

static void test_a_field_read_names_only_what_that_field_was_given() {
    assert(test_compiles("struct Node { n: int }\n"
                         "struct Pair { a: &Node, b: &Node }\n"
                         "func f(p: &Node): &Node {\n"
                         "    let l = Node { n: 1 };\n"
                         "    let x = Pair { a: p, b: l };\n"
                         "    return x.a;\n"
                         "}\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "struct Pair { a: &Node, b: &Node }\n"
                          "func f(p: &Node): &Node {\n"
                          "    let l = Node { n: 1 };\n"
                          "    let x = Pair { a: l, b: p };\n"
                          "    return x.a;\n"
                          "}\n"));
}

static void test_a_field_of_a_field_names_only_what_it_was_given() {
    assert(test_compiles("struct Node { n: int }\n"
                         "struct View { r: &Node }\n"
                         "struct Two { u: View, v: View }\n"
                         "func f(p: &Node): &Node {\n"
                         "    let l = Node { n: 1 };\n"
                         "    let t = Two { u: View { r: p }, v: View { r: l } };\n"
                         "    return t.u.r;\n"
                         "}\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "struct View { r: &Node }\n"
                          "struct Two { u: View, v: View }\n"
                          "func f(p: &Node): &Node {\n"
                          "    let l = Node { n: 1 };\n"
                          "    let t = Two { u: View { r: l }, v: View { r: p } };\n"
                          "    return t.u.r;\n"
                          "}\n"));
}

static void test_a_whole_struct_names_every_field_it_holds() {
    assert(!test_compiles("struct Node { n: int }\n"
                          "struct Pair { a: &Node, b: &Node }\n"
                          "func f(p: &Node): Pair {\n"
                          "    let l = Node { n: 1 };\n"
                          "    let x = Pair { a: p, b: l };\n"
                          "    return x;\n"
                          "}\n"));
}

static void test_writing_one_field_leaves_the_others_as_they_were() {
    assert(test_compiles("struct Node { n: int }\n"
                         "struct Pair { a: &Node, b: &Node }\n"
                         "func f(p: &Node): &Node {\n"
                         "    let l = Node { n: 1 };\n"
                         "    let x = Pair { a: p, b: p };\n"
                         "    x.b = l;\n"
                         "    return x.a;\n"
                         "}\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "struct Pair { a: &Node, b: &Node }\n"
                          "func f(p: &Node, q: &Node): &Node {\n"
                          "    let l = Node { n: 1 };\n"
                          "    let x = Pair { a: p, b: q };\n"
                          "    x.b = l;\n"
                          "    return x.b;\n"
                          "}\n"));
}

static void test_freeing_a_box_dangles_only_the_fields_that_named_it() {
    assert(test_compiles("struct Node { n: int }\n"
                         "struct Pair { a: &Node, b: &Node }\n"
                         "func f(): int {\n"
                         "    let p: *Node = box Node { n: 1 };\n"
                         "    let q: *Node = box Node { n: 2 };\n"
                         "    let x = Pair { a: p, b: q };\n"
                         "    p = box Node { n: 3 };\n"
                         "    return x.b.n;\n"
                         "}\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "struct Pair { a: &Node, b: &Node }\n"
                          "func f(): int {\n"
                          "    let p: *Node = box Node { n: 1 };\n"
                          "    let q: *Node = box Node { n: 2 };\n"
                          "    let x = Pair { a: p, b: q };\n"
                          "    p = box Node { n: 3 };\n"
                          "    return x.a.n;\n"
                          "}\n"));

    assert(!test_compiles("struct Node { n: int }\n"
                          "struct Pair { a: &Node, b: &Node }\n"
                          "func g(v: Pair): int { return 0; }\n"
                          "func f(): int {\n"
                          "    let p: *Node = box Node { n: 1 };\n"
                          "    let q: *Node = box Node { n: 2 };\n"
                          "    let x = Pair { a: p, b: q };\n"
                          "    p = box Node { n: 3 };\n"
                          "    return g(x);\n"
                          "}\n"));
}

int main(void) {
    test_a_struct_holding_a_borrow_of_a_local_is_not_returned();
    test_a_borrow_nested_deeper_than_one_struct_is_still_seen();
    test_a_struct_of_borrows_assigned_outward_names_what_outlives_it();
    test_a_struct_that_owns_its_field_is_free_to_leave();
    test_a_struct_of_borrows_lives_as_long_as_what_it_names();
    test_a_returned_struct_of_borrows_names_the_arguments_it_came_from();
    test_a_returned_struct_that_owns_is_free_of_its_arguments();
    test_a_returned_struct_names_only_the_arguments_it_reaches();
    test_a_field_holds_a_borrow_as_long_as_the_struct_it_sits_in();
    test_a_field_reached_through_a_heap_slot_outlives_every_scope();
    test_a_field_given_a_borrow_narrows_the_struct_that_holds_it();
    test_a_field_read_names_only_what_that_field_was_given();
    test_a_field_of_a_field_names_only_what_it_was_given();
    test_a_whole_struct_names_every_field_it_holds();
    test_writing_one_field_leaves_the_others_as_they_were();
    test_freeing_a_box_dangles_only_the_fields_that_named_it();
    return 0;
}
