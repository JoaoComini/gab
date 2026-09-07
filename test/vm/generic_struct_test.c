#include "support/run.h"

#include <assert.h>

static void test_a_generic_field_holds_its_argument() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func f(): i32 { let h = Holder<i32> { value: 7 }; return h.value; }\n"
                        "let r: i32 = f();") == 7);
}

static void test_two_instantiations_are_two_types() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): i32 { let h = Holder<i32> { value: 1.5 }; return h.value; }\n"
                          "let r: i32 = f();"));
}

static void test_the_bare_declaration_names_no_type() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): i32 { let h: Holder; return 1; }\n"
                          "let r: i32 = f();"));
}

static void test_a_mention_owes_every_argument() {
    assert(!test_compiles("struct Pair<A, B> { first: A, second: B }\n"
                          "func f(): i32 { let p: Pair<i32>; return 1; }\n"
                          "let r: i32 = f();"));
}

static void test_a_parameter_names_nothing_outside_its_declaration() {
    assert(!test_compiles("func f(): i32 { let x: T; return 1; }\n"
                          "let r: i32 = f();"));
}

static void test_a_parameter_nests_under_a_constructor() {
    assert(test_run_int("struct Cell { n: i32 }\n"
                        "struct Holder<T> { value: *T }\n"
                        "func f(): i32 { let h = Holder<Cell> { value: box Cell { n: 0 } }; h.value.n = 9;\n"
                        "return h.value.n; }\n"
                        "let r: i32 = f();") == 9);
}

static void test_each_argument_reaches_its_own_parameter() {
    assert(test_run_int("struct Pair<A, B> { first: A, second: B }\n"
                        "func f(): i32 { let p = Pair<i32, bool> { first: 4, second: true };\n"
                        "if p.second { return p.first; } return 0; }\n"
                        "let r: i32 = f();") == 4);
}

static void test_a_declaration_reaches_itself_through_an_indirection() {
    assert(test_run_int("struct Node<T> { value: T, next: *Leaf<T> }\n"
                        "struct Leaf<T> { value: T }\n"
                        "func f(): i32 { let n = Node<i32> { value: 5, next: box Leaf<i32> { value: 0 } };\n"
                        "return n.value; }\n"
                        "let r: i32 = f();") == 5);
}

static void test_a_declaration_reaches_one_declared_below_it() {
    assert(test_run_int("struct Outer<T> { inner: *Inner<T> }\n"
                        "struct Inner<T> { value: T }\n"
                        "func f(): i32 { let o = Outer<i32> { inner: box Inner<i32> { value: 0 } };\n"
                        "o.inner.value = 6; return o.inner.value; }\n"
                        "let r: i32 = f();") == 6);
}

static void test_a_declaration_holds_a_function_over_its_parameters() {
    assert(test_compiles("struct Holder<T> { value: T }\n"
                         "impl<T> Holder<T> {\n"
                         "    extern func get(h: &Holder<T>): T;\n"
                         "}\n"
                         "func f(): i32 { return 1; }\n"
                         "let r: i32 = f();"));
}

static void test_a_parameter_names_the_declaration_it_belongs_to() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "impl<U> Holder<U> {\n"
                          "    extern func get(h: &Holder<T>): i32;\n"
                          "}\n"
                          "func f(): i32 { return 1; }\n"
                          "let r: i32 = f();"));
}

static void test_a_supplied_argument_owes_a_width() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): i32 { let h: Holder<str>; return 1; }\n"
                          "let r: i32 = f();"));
}

static void test_a_declaration_owed_arguments_holds_no_function() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): i32 { return Holder::get(); }\n"
                          "let r: i32 = f();"));
}

static void test_a_declaration_owed_arguments_converts_nothing() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): i32 { return Holder(1); }\n"
                          "let r: i32 = f();"));
}

static void test_a_parameter_is_the_element_of_an_array() {
    assert(test_run_int("struct Buf<T> { xs: array<T, 3> }\n"
                        "func f(): i32 { let b = Buf<i32> { xs: [0, 9, 0] }; return b.xs[1]; }\n"
                        "let r: i32 = f();") == 9);
}

static void test_a_function_owned_by_an_instantiation_is_called_through_it() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "impl<T> Holder<T> {\n"
                        "    func make(n: i32): i32 { return n + 1; }\n"
                        "}\n"
                        "func f(): i32 { return Holder<i32>::make(6); }\n"
                        "let r: i32 = f();") == 7);
}

static void test_a_method_on_a_declaration_serves_an_instantiation() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "impl<T> Holder<T> {\n"
                        "    func get(h: &Holder<T>): T { return h.value; }\n"
                        "}\n"
                        "func f(): i32 { let h = Holder<i32> { value: 3 }; return h.get(); }\n"
                        "let r: i32 = f();") == 3);
}

static void test_a_parameter_of_the_impl_reaches_a_method_argument() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "impl<T> Holder<T> {\n"
                        "    func set(h: &Holder<T>, v: T): i32 { h.value = v; return 0; }\n"
                        "}\n"
                        "func f(): i32 { let h = Holder<i32> { value: 3 }; h.set(9); return h.value; }\n"
                        "let r: i32 = f();") == 9);
}

static void test_each_instantiation_gets_its_own_body() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "impl<T> Holder<T> {\n"
                        "    func get(h: &Holder<T>): T { return h.value; }\n"
                        "}\n"
                        "func f(): i32 { let a = Holder<i32> { value: 4 };\n"
                        "let b = Holder<bool> { value: true };\n"
                        "if b.get() { return a.get(); } return 0; }\n"
                        "let r: i32 = f();") == 4);
}

static void test_an_instantiation_that_owns_frees_what_it_holds() {
    assert(test_run_int("struct Cell { n: i32 }\n"
                        "struct Holder<T> { value: T }\n"
                        "impl<T> Holder<T> {\n"
                        "    func read(h: &Holder<T>): i32 { return 1; }\n"
                        "}\n"
                        "func f(): i32 { let h = Holder<*Cell> { value: box Cell { n: 0 } };\n"
                        "h.value.n = 8; return h.read() + h.value.n; }\n"
                        "let r: i32 = f();") == 9);
}

static void test_an_instantiation_reached_from_another_is_emitted() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "struct Wrap<T> { inner: Holder<T> }\n"
                        "impl<T> Holder<T> {\n"
                        "    func get(h: &Holder<T>): T { return h.value; }\n"
                        "}\n"
                        "impl<T> Wrap<T> {\n"
                        "    func unwrap(w: &Wrap<T>): T { return w.inner.get(); }\n"
                        "}\n"
                        "func f(): i32 { let w = Wrap<i32> { inner: Holder<i32> { value: 6 } };\n"
                        "return w.unwrap(); }\n"
                        "let r: i32 = f();") == 6);
}

static void test_a_generic_that_instantiates_itself_is_rejected() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "impl<T> Holder<T> {\n"
                          "    func deeper(h: &Holder<T>): i32 {\n"
                          "        let n: Holder<*T>; return n.deeper(); }\n"
                          "}\n"
                          "func f(): i32 { let h = Holder<i32> { value: 0 }; return h.deeper(); }\n"
                          "let r: i32 = f();"));
}

static void test_a_free_function_takes_type_parameters() {
    assert(test_run_int("func id<T>(x: T): T { return x; }\n"
                        "func f(): i32 { return id<i32>(7); }\n"
                        "let r: i32 = f();") == 7);
}

static void test_each_instantiation_of_a_free_function_takes_its_own_type() {
    assert(!test_compiles("func id<T>(x: T): T { return x; }\n"
                          "func f(): i32 { return id<bool>(7); }\n"
                          "let r: i32 = f();"));
}

static void test_a_free_function_owes_every_type_argument() {
    assert(!test_compiles("func pair<A, B>(a: A, b: B): A { return a; }\n"
                          "func f(): i32 { return pair<i32>(1, 2); }\n"
                          "let r: i32 = f();"));
}

static void test_an_argument_names_the_type_parameter_it_fills() {
    assert(test_run_int("func id<T>(x: T): T { return x; }\n"
                        "func f(): i32 { return id(7); }\n"
                        "let r: i32 = f();") == 7);
}

static void test_a_type_parameter_no_argument_reaches_is_written() {
    assert(!test_compiles("func make<T>(n: i32): i32 { return n; }\n"
                          "func f(): i32 { return make(1); }\n"
                          "let r: i32 = f();"));
}

static void test_one_type_parameter_reached_two_ways_takes_one_type() {
    assert(!test_compiles("struct Holder<T> { v: T }\n"
                          "func both<T>(a: Holder<T>, b: T): i32 { return 1; }\n"
                          "func f(): i32 { let h = Holder<i32> { v: 1 }; return both(h, true); }\n"
                          "let r: i32 = f();"));
}

static void test_one_parameter_filled_twice_takes_one_type() {
    assert(!test_compiles("func pick<T>(a: T, b: T): T { return a; }\n"
                          "func f(): i32 { return pick(1, true); }\n"
                          "let r: i32 = f();"));
}

static void test_two_instantiations_of_a_free_function_each_run() {
    assert(test_run_int("func id<T>(x: T): T { return x; }\n"
                        "func f(): i32 { if id<bool>(true) { return id<i32>(4); } return 0; }\n"
                        "let r: i32 = f();") == 4);
}

static void test_an_inferred_call_borrows_its_argument() {
    assert(test_run_int("struct Cell { n: i32 }\n"
                        "func read<T>(c: &T): i32 { return 1; }\n"
                        "func f(): i32 { let o: *Cell = box Cell { n: 5 };\n"
                        "return read(o) + o.n; }\n"
                        "let r: i32 = f();") == 6);
}

static void test_an_inferred_call_moves_what_it_is_given() {
    assert(test_run_int("struct Cell { n: i32 }\n"
                        "func take<T>(c: *T): i32 { return 2; }\n"
                        "func f(): i32 { let o: *Cell = box Cell { n: 0 };\n"
                        "return take(o); }\n"
                        "let r: i32 = f();") == 2);
}

int main() {
    test_an_inferred_call_borrows_its_argument();
    test_an_inferred_call_moves_what_it_is_given();
    test_a_free_function_takes_type_parameters();
    test_each_instantiation_of_a_free_function_takes_its_own_type();
    test_a_free_function_owes_every_type_argument();
    test_an_argument_names_the_type_parameter_it_fills();
    test_a_type_parameter_no_argument_reaches_is_written();
    test_one_type_parameter_reached_two_ways_takes_one_type();
    test_one_parameter_filled_twice_takes_one_type();
    test_two_instantiations_of_a_free_function_each_run();
    test_a_generic_that_instantiates_itself_is_rejected();
    test_an_instantiation_reached_from_another_is_emitted();
    test_an_instantiation_that_owns_frees_what_it_holds();
    test_a_parameter_of_the_impl_reaches_a_method_argument();
    test_each_instantiation_gets_its_own_body();
    test_a_function_owned_by_an_instantiation_is_called_through_it();
    test_a_method_on_a_declaration_serves_an_instantiation();
    test_a_generic_field_holds_its_argument();
    test_two_instantiations_are_two_types();
    test_the_bare_declaration_names_no_type();
    test_a_mention_owes_every_argument();
    test_a_parameter_names_nothing_outside_its_declaration();
    test_a_parameter_nests_under_a_constructor();
    test_a_parameter_is_the_element_of_an_array();
    test_each_argument_reaches_its_own_parameter();
    test_a_declaration_reaches_itself_through_an_indirection();
    test_a_declaration_reaches_one_declared_below_it();
    test_a_declaration_holds_a_function_over_its_parameters();
    test_a_parameter_names_the_declaration_it_belongs_to();
    test_a_supplied_argument_owes_a_width();
    test_a_declaration_owed_arguments_holds_no_function();
    test_a_declaration_owed_arguments_converts_nothing();

    return 0;
}
