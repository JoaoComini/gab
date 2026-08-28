// Functions a type owns rather than ones a value reaches: declared
// 'func Type::name(...)' and called the same way.

#include "support/run.h"

#include <assert.h>
#include <stdio.h>

// A function declared with '::' takes no receiver and is called on its type.
static void test_a_static_function_is_called_on_its_type() {
    assert(test_run_int("struct Counter { n: int }\n"
                        "func Counter::start(): int { return 7; }\n"
                        "func main(): int { return Counter::start(); }\n"
                        "let r: int = main();") == 7);
}

// Every parameter is one a call writes, since none of them is a receiver.
static void test_a_static_function_takes_its_declared_parameters() {
    assert(test_run_int("struct Counter { n: int }\n"
                        "func Counter::of(a: int, b: int): int { return a + b; }\n"
                        "func main(): int { return Counter::of(3, 4); }\n"
                        "let r: int = main();") == 7);
}

// A method needs a receiver, which the type spelling has no way to supply.
static void test_a_method_is_not_reached_on_the_type() {
    assert(!test_compiles("struct Counter { n: int }\n"
                          "func (c: ref Counter) get(): int { return c.n; }\n"
                          "func main(): int { return Counter::get(); }\n"));
}

// The mirror: a static belongs to the type, so no value answers to it.
static void test_a_static_function_is_not_reached_on_a_value() {
    assert(!test_compiles("struct Counter { n: int }\n"
                          "func Counter::start(): int { return 7; }\n"
                          "func main(): int { let c: Counter; return c.start(); }\n"));

    // One that declares parameters is still not reached through a value: what
    // rules it out is having no receiver, not having nothing at all.
    assert(!test_compiles("struct Counter { n: int }\n"
                          "func Counter::of(a: int): int { return a; }\n"
                          "func main(): int { let c: Counter; return c.of(1); }\n"));
}

// A name the type does not answer to is reported against the type.
static void test_an_unknown_name_on_a_type_is_rejected() {
    assert(!test_compiles("struct Counter { n: int }\n"
                          "func main(): int { return Counter::missing(); }\n"));
}

// A type this module does not declare is not one it may extend.
static void test_a_static_function_needs_a_type_it_declares() {
    assert(!test_compiles("func Missing::start(): int { return 7; }\n"));
}

// A receiver says the function is reached through a value and an owner says it
// is not, so a declaration carrying both says nothing.
static void test_a_receiver_and_an_owner_do_not_combine() {
    assert(!test_compiles("struct Counter { n: int }\n"
                          "func (c: ref Counter) Counter::start(): int { return 7; }\n"));
}

int main(void) {
    test_a_static_function_is_called_on_its_type();
    test_a_static_function_takes_its_declared_parameters();
    test_a_method_is_not_reached_on_the_type();
    test_a_static_function_is_not_reached_on_a_value();
    test_an_unknown_name_on_a_type_is_rejected();
    test_a_static_function_needs_a_type_it_declares();
    test_a_receiver_and_an_owner_do_not_combine();

    printf("All static function tests passed\n");
    return 0;
}
