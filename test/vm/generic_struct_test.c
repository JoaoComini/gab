#include "support/run.h"

#include <assert.h>

static void test_a_generic_field_holds_its_argument() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func f(): int { let h = Holder<int> { value: 7 }; return h.value; }\n"
                        "let r: int = f();") == 7);
}

static void test_two_instantiations_are_two_types() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { let h = Holder<int> { value: 1.5 }; return h.value; }\n"
                          "let r: int = f();"));
}

static void test_the_bare_declaration_names_no_type() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { let h: Holder; return 1; }\n"
                          "let r: int = f();"));
}

static void test_a_mention_owes_every_argument() {
    assert(!test_compiles("struct Pair<A, B> { first: A, second: B }\n"
                          "func f(): int { let p: Pair<int>; return 1; }\n"
                          "let r: int = f();"));
}

static void test_a_parameter_names_nothing_outside_its_declaration() {
    assert(!test_compiles("func f(): int { let x: T; return 1; }\n"
                          "let r: int = f();"));
}

static void test_a_parameter_nests_under_a_constructor() {
    assert(test_run_int("struct Cell { n: int }\n"
                        "struct Holder<T> { value: *T }\n"
                        "func f(): int { let h = Holder<Cell> { value: box Cell { n: 0 } }; h.value.n = 9;\n"
                        "return h.value.n; }\n"
                        "let r: int = f();") == 9);
}

static void test_each_argument_reaches_its_own_parameter() {
    assert(test_run_int("struct Pair<A, B> { first: A, second: B }\n"
                        "func f(): int { let p = Pair<int, bool> { first: 4, second: true };\n"
                        "if p.second { return p.first; } return 0; }\n"
                        "let r: int = f();") == 4);
}

static void test_a_declaration_reaches_itself_through_an_indirection() {
    assert(test_run_int("struct Node<T> { value: T, next: *Leaf<T> }\n"
                        "struct Leaf<T> { value: T }\n"
                        "func f(): int { let n = Node<int> { value: 5, next: box Leaf<int> { value: 0 } };\n"
                        "return n.value; }\n"
                        "let r: int = f();") == 5);
}

static void test_a_declaration_reaches_one_declared_below_it() {
    assert(test_run_int("struct Outer<T> { inner: *Inner<T> }\n"
                        "struct Inner<T> { value: T }\n"
                        "func f(): int { let o = Outer<int> { inner: box Inner<int> { value: 0 } };\n"
                        "o.inner.value = 6; return o.inner.value; }\n"
                        "let r: int = f();") == 6);
}

static void test_a_declaration_holds_a_function_over_its_parameters() {
    assert(test_compiles("struct Holder<T> { value: T }\n"
                         "extern func Holder<T>::get(h: &Holder<T>): T;\n"
                         "func f(): int { return 1; }\n"
                         "let r: int = f();"));
}

static void test_a_parameter_names_the_declaration_it_belongs_to() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "extern func Holder<U>::get(h: &Holder<T>): int;\n"
                          "func f(): int { return 1; }\n"
                          "let r: int = f();"));
}

static void test_a_supplied_argument_owes_a_width() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { let h: Holder<str>; return 1; }\n"
                          "let r: int = f();"));
}

static void test_a_declaration_owed_arguments_holds_no_function() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { return Holder::get(); }\n"
                          "let r: int = f();"));
}

static void test_a_declaration_owed_arguments_converts_nothing() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { return Holder(1); }\n"
                          "let r: int = f();"));
}

static void test_a_parameter_is_the_element_of_an_array() {
    assert(test_run_int("struct Buf<T> { xs: [T; 3] }\n"
                        "func f(): int { let b = Buf<int> { xs: [0, 9, 0] }; return b.xs[1]; }\n"
                        "let r: int = f();") == 9);
}

static void test_a_function_owned_by_an_instantiation_is_called_through_it() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func Holder<T>::make(n: int): int { return n + 1; }\n"
                        "func f(): int { return Holder<int>::make(6); }\n"
                        "let r: int = f();") == 7);
}

static void test_a_method_on_a_declaration_serves_an_instantiation() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func Holder<T>::get(h: &Holder<T>): T { return h.value; }\n"
                        "func f(): int { let h = Holder<int> { value: 3 }; return h.get(); }\n"
                        "let r: int = f();") == 3);
}

static void test_each_instantiation_gets_its_own_body() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func Holder<T>::get(h: &Holder<T>): T { return h.value; }\n"
                        "func f(): int { let a = Holder<int> { value: 4 };\n"
                        "let b = Holder<bool> { value: true };\n"
                        "if b.get() { return a.get(); } return 0; }\n"
                        "let r: int = f();") == 4);
}

static void test_an_instantiation_that_owns_frees_what_it_holds() {
    assert(test_run_int("struct Cell { n: int }\n"
                        "struct Holder<T> { value: T }\n"
                        "func Holder<T>::read(h: &Holder<T>): int { return 1; }\n"
                        "func f(): int { let h = Holder<*Cell> { value: box Cell { n: 0 } };\n"
                        "h.value.n = 8; return h.read() + h.value.n; }\n"
                        "let r: int = f();") == 9);
}

static void test_an_instantiation_reached_from_another_is_emitted() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "struct Wrap<T> { inner: Holder<T> }\n"
                        "func Holder<T>::get(h: &Holder<T>): T { return h.value; }\n"
                        "func Wrap<T>::unwrap(w: &Wrap<T>): T { return w.inner.get(); }\n"
                        "func f(): int { let w = Wrap<int> { inner: Holder<int> { value: 6 } };\n"
                        "return w.unwrap(); }\n"
                        "let r: int = f();") == 6);
}

static void test_a_generic_that_instantiates_itself_is_rejected() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func Holder<T>::deeper(h: &Holder<T>): int {\n"
                          "    let n: Holder<*T>; return n.deeper(); }\n"
                          "func f(): int { let h = Holder<int> { value: 0 }; return h.deeper(); }\n"
                          "let r: int = f();"));
}

static void test_a_free_function_takes_type_parameters() {
    assert(test_run_int("func id<T>(x: T): T { return x; }\n"
                        "func f(): int { return id<int>(7); }\n"
                        "let r: int = f();") == 7);
}

static void test_each_instantiation_of_a_free_function_takes_its_own_type() {
    assert(!test_compiles("func id<T>(x: T): T { return x; }\n"
                          "func f(): int { return id<bool>(7); }\n"
                          "let r: int = f();"));
}

static void test_a_free_function_owes_every_type_argument() {
    assert(!test_compiles("func pair<A, B>(a: A, b: B): A { return a; }\n"
                          "func f(): int { return pair<int>(1, 2); }\n"
                          "let r: int = f();"));
}

static void test_an_argument_names_the_type_parameter_it_fills() {
    assert(test_run_int("func id<T>(x: T): T { return x; }\n"
                        "func f(): int { return id(7); }\n"
                        "let r: int = f();") == 7);
}

static void test_a_type_parameter_no_argument_reaches_is_written() {
    assert(!test_compiles("func make<T>(n: int): int { return n; }\n"
                          "func f(): int { return make(1); }\n"
                          "let r: int = f();"));
}

static void test_one_type_parameter_reached_two_ways_takes_one_type() {
    assert(!test_compiles("struct Holder<T> { v: T }\n"
                          "func both<T>(a: Holder<T>, b: T): int { return 1; }\n"
                          "func f(): int { let h = Holder<int> { v: 1 }; return both(h, true); }\n"
                          "let r: int = f();"));
}

static void test_one_parameter_filled_twice_takes_one_type() {
    assert(!test_compiles("func pick<T>(a: T, b: T): T { return a; }\n"
                          "func f(): int { return pick(1, true); }\n"
                          "let r: int = f();"));
}

static void test_two_instantiations_of_a_free_function_each_run() {
    assert(test_run_int("func id<T>(x: T): T { return x; }\n"
                        "func f(): int { if id<bool>(true) { return id<int>(4); } return 0; }\n"
                        "let r: int = f();") == 4);
}

static void test_an_inferred_call_borrows_its_argument() {
    assert(test_run_int("struct Cell { n: int }\n"
                        "func read<T>(c: &T): int { return 1; }\n"
                        "func f(): int { let o: *Cell = box Cell { n: 5 };\n"
                        "return read(o) + o.n; }\n"
                        "let r: int = f();") == 6);
}

static void test_an_inferred_call_moves_what_it_is_given() {
    assert(test_run_int("struct Cell { n: int }\n"
                        "func take<T>(c: *T): int { return 2; }\n"
                        "func f(): int { let o: *Cell = box Cell { n: 0 };\n"
                        "return take(o); }\n"
                        "let r: int = f();") == 2);
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
