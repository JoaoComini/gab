#include "support/run.h"

#include <assert.h>

static void test_a_generic_field_holds_its_argument() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func f(): int { let h: Holder<int>; h.value = 7; return h.value; }\n"
                        "let r: int = f();") == 7);
}

static void test_two_instantiations_are_two_types() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { let h: Holder<int>; h.value = 1.5; return h.value; }\n"
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
                        "struct Holder<T> { value: box T }\n"
                        "func f(): int { let h: Holder<Cell>; h.value = new Cell; h.value.n = 9;\n"
                        "return h.value.n; }\n"
                        "let r: int = f();") == 9);
}

static void test_each_argument_reaches_its_own_parameter() {
    assert(test_run_int("struct Pair<A, B> { first: A, second: B }\n"
                        "func f(): int { let p: Pair<int, bool>; p.first = 4; p.second = true;\n"
                        "if p.second { return p.first; } return 0; }\n"
                        "let r: int = f();") == 4);
}

static void test_a_declaration_reaches_itself_through_an_indirection() {
    assert(test_run_int("struct Node<T> { value: T, next: box Node<T> }\n"
                        "func f(): int { let n: Node<int>; n.value = 5; return n.value; }\n"
                        "let r: int = f();") == 5);
}

static void test_a_declaration_reaches_one_declared_below_it() {
    assert(test_run_int("struct Outer<T> { inner: box Inner<T> }\n"
                        "struct Inner<T> { value: T }\n"
                        "func f(): int { let o: Outer<int>; o.inner = new Inner<int>;\n"
                        "o.inner.value = 6; return o.inner.value; }\n"
                        "let r: int = f();") == 6);
}

static void test_a_declaration_holds_a_function_over_its_parameters() {
    assert(test_compiles("struct Holder<T> { value: T }\n"
                         "extern func Holder<T>::get(h: ref Holder<T>): T;\n"
                         "func f(): int { return 1; }\n"
                         "let r: int = f();"));
}

static void test_a_parameter_names_the_declaration_it_belongs_to() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "extern func Holder<U>::get(h: ref Holder<T>): int;\n"
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
                        "func f(): int { let b: Buf<int>; b.xs[1] = 9; return b.xs[1]; }\n"
                        "let r: int = f();") == 9);
}

static void test_a_method_on_a_declaration_serves_an_instantiation() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func Holder<T>::get(h: ref Holder<T>): T { return h.value; }\n"
                        "func f(): int { let h: Holder<int>; h.value = 3; return h.get(); }\n"
                        "let r: int = f();") == 3);
}

static void test_each_instantiation_gets_its_own_body() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func Holder<T>::get(h: ref Holder<T>): T { return h.value; }\n"
                        "func f(): int { let a: Holder<int>; a.value = 4;\n"
                        "let b: Holder<bool>; b.value = true;\n"
                        "if b.get() { return a.get(); } return 0; }\n"
                        "let r: int = f();") == 4);
}

static void test_an_instantiation_that_owns_frees_what_it_holds() {
    assert(test_run_int("struct Cell { n: int }\n"
                        "struct Holder<T> { value: T }\n"
                        "func Holder<T>::read(h: ref Holder<T>): int { return 1; }\n"
                        "func f(): int { let h: Holder<box Cell>; h.value = new Cell;\n"
                        "h.value.n = 8; return h.read() + h.value.n; }\n"
                        "let r: int = f();") == 9);
}

static void test_an_instantiation_reached_from_another_is_emitted() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "struct Wrap<T> { inner: Holder<T> }\n"
                        "func Holder<T>::get(h: ref Holder<T>): T { return h.value; }\n"
                        "func Wrap<T>::unwrap(w: ref Wrap<T>): T { return w.inner.get(); }\n"
                        "func f(): int { let w: Wrap<int>; w.inner.value = 6; return w.unwrap(); }\n"
                        "let r: int = f();") == 6);
}

int main() {
    test_an_instantiation_reached_from_another_is_emitted();
    test_an_instantiation_that_owns_frees_what_it_holds();
    test_each_instantiation_gets_its_own_body();
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
