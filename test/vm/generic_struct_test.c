#include "support/run.h"

#include <assert.h>

// A declaration taking a parameter is instantiated by what a mention applies it
// to, and the field written over that parameter holds the argument's type.
static void test_a_generic_field_holds_its_argument() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "func f(): int { let h: Holder<int>; h.value = 7; return h.value; }\n"
                        "let r: int = f();") == 7);
}

// Two instantiations of one declaration are two types, so what one holds is not
// what the other does.
static void test_two_instantiations_are_two_types() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { let h: Holder<int>; h.value = 1.5; return h.value; }\n"
                          "let r: int = f();"));
}

// A declaration names no type until a mention supplies its arguments.
static void test_the_bare_declaration_names_no_type() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { let h: Holder; return 1; }\n"
                          "let r: int = f();"));
}

// The count a declaration takes is what every mention owes it.
static void test_a_mention_owes_every_argument() {
    assert(!test_compiles("struct Pair<A, B> { first: A, second: B }\n"
                          "func f(): int { let p: Pair<int>; return 1; }\n"
                          "let r: int = f();"));
}

// A parameter belongs to the declaration that takes it, and names nothing
// outside one.
static void test_a_parameter_names_nothing_outside_its_declaration() {
    assert(!test_compiles("func f(): int { let x: T; return 1; }\n"
                          "let r: int = f();"));
}

// A field written over a parameter nests: the constructor around it is rebuilt
// with the argument in place.
static void test_a_parameter_nests_under_a_constructor() {
    assert(test_run_int("struct Cell { n: int }\n"
                        "struct Holder<T> { value: box T }\n"
                        "func f(): int { let h: Holder<Cell>; h.value = new Cell; h.value.n = 9;\n"
                        "return h.value.n; }\n"
                        "let r: int = f();") == 9);
}

// Each argument reaches the parameter at its own index.
static void test_each_argument_reaches_its_own_parameter() {
    assert(test_run_int("struct Pair<A, B> { first: A, second: B }\n"
                        "func f(): int { let p: Pair<int, bool>; p.first = 4; p.second = true;\n"
                        "if p.second { return p.first; } return 0; }\n"
                        "let r: int = f();") == 4);
}

// A declaration's field may name the declaration it belongs to, through an
// indirection: the name is bound before any field resolves, so what it names is
// reachable while its own fields are still being read.
static void test_a_declaration_reaches_itself_through_an_indirection() {
    assert(test_run_int("struct Node<T> { value: T, next: box Node<T> }\n"
                        "func f(): int { let n: Node<int>; n.value = 5; return n.value; }\n"
                        "let r: int = f();") == 5);
}

// A declaration's field may name one declared below it, for the same reason.
static void test_a_declaration_reaches_one_declared_below_it() {
    assert(test_run_int("struct Outer<T> { inner: box Inner<T> }\n"
                        "struct Inner<T> { value: T }\n"
                        "func f(): int { let o: Outer<int>; o.inner = new Inner<int>;\n"
                        "o.inner.value = 6; return o.inner.value; }\n"
                        "let r: int = f();") == 6);
}

// A function belongs to a type, and a declaration taking parameters is not one:
// which instantiation would own it is not settled by the name alone.
static void test_a_function_belongs_to_an_instantiation_not_a_declaration() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func Holder::get(h: ref Holder<int>): int { return h.value; }\n"
                          "func f(): int { return 1; }\n"
                          "let r: int = f();"));
}

// An argument still owes a width where one is actually supplied: passing a
// parameter through is what a declaration's own field does, and it settles
// nothing about what may reach a slot.
static void test_a_supplied_argument_owes_a_width() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { let h: Holder<str>; return 1; }\n"
                          "let r: int = f();"));
}

// A declaration still owed arguments owns no function set: which instantiation
// 'Holder::get' meant is what a mention supplies.
static void test_a_declaration_owed_arguments_holds_no_function() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { return Holder::get(); }\n"
                          "let r: int = f();"));
}

// A conversion is spelled like a call, and which one a name is depends on what
// that name means: a declaration owed arguments converts nothing.
static void test_a_declaration_owed_arguments_converts_nothing() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "func f(): int { return Holder(1); }\n"
                          "let r: int = f();"));
}

// An array of a parameter has no width where it is written, only where a
// mention supplies one: the run is laid out from whatever the argument turns
// out to be.
static void test_a_parameter_is_the_element_of_an_array() {
    assert(test_run_int("struct Buf<T> { xs: [T; 3] }\n"
                        "func f(): int { let b: Buf<int>; b.xs[1] = 9; return b.xs[1]; }\n"
                        "let r: int = f();") == 9);
}

int main() {
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
    test_a_function_belongs_to_an_instantiation_not_a_declaration();
    test_a_supplied_argument_owes_a_width();
    test_a_declaration_owed_arguments_holds_no_function();
    test_a_declaration_owed_arguments_converts_nothing();

    return 0;
}
