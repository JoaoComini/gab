// Functions declared into a type's own set, and the sugar that reaches one
// through a value: 'v.name(a)' is 'Type::name(v, a)'.

#include "support/run.h"

#include <assert.h>
#include <stdio.h>

// A function a type owns is called on the type.
static void test_a_function_is_called_on_its_type() {
    assert(test_run_int("struct Counter { n: int }\n"
                        "func Counter::start(): int { return 7; }\n"
                        "func main(): int { return Counter::start(); }\n"
                        "let r: int = main();") == 7);
}

static void test_a_function_takes_its_declared_parameters() {
    assert(test_run_int("struct Counter { n: int }\n"
                        "func Counter::of(a: int, b: int): int { return a + b; }\n"
                        "func main(): int { return Counter::of(3, 4); }\n"
                        "let r: int = main();") == 7);
}

// Parameter zero is an ordinary parameter, so a function taking the owning type
// first is reached either way.
static void test_the_sugar_fills_parameter_zero() {
    assert(test_run_int("struct Counter { n: int }\n"
                        "func Counter::of(c: ref Counter): int { return c.n; }\n"
                        "func main(): int {\n"
                        "    let c: Counter;\n"
                        "    c.n = 5;\n"
                        "    return c.of() + Counter::of(c);\n"
                        "}\n"
                        "let r: int = main();") == 10);
}

// The sugar has nothing to fill when a function declares no parameters.
static void test_a_function_taking_nothing_is_not_reached_on_a_value() {
    assert(!test_compiles("struct Counter { n: int }\n"
                          "func Counter::start(): int { return 7; }\n"
                          "func main(): int { let c: Counter; return c.start(); }\n"));
}

// A name the type does not answer to is reported against the type.
static void test_an_unknown_name_on_a_type_is_rejected() {
    assert(!test_compiles("struct Counter { n: int }\n"
                          "func main(): int { return Counter::missing(); }\n"));
}

// A type this module does not declare is not one it may extend.
static void test_a_function_needs_a_type_its_module_declares() {
    assert(!test_compiles("func Missing::start(): int { return 7; }\n"));

    assert(!test_compiles("func int::double(n: int): int { return n; }\n"));
}

// A function a type owns may consume what it is given: parameter zero is an
// ordinary parameter, so the call site hands its argument over.
static void test_a_function_consumes_what_it_is_given() {
    assert(test_run_int("struct Holder { n: int }\n"
                        "func Holder::take(h: box Holder): int { return h.n; }\n"
                        "func main(): int {\n"
                        "    let h: box Holder = new Holder;\n"
                        "    h.n = 6;\n"
                        "    return Holder::take(h);\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

// A function consuming parameter zero is reached on a value like any other: the
// sugar hands the receiver over, and the slot it came from is dead after.
static void test_a_consuming_function_is_reached_through_a_value() {
    assert(test_run_int("struct Holder { n: int }\n"
                        "func Holder::take(h: box Holder): int { return h.n; }\n"
                        "func main(): int {\n"
                        "    let h: box Holder = new Holder;\n"
                        "    h.n = 4;\n"
                        "    return h.take();\n"
                        "}\n"
                        "let r: int = main();") == 4);

    // What it took is gone: the receiver was handed over, not lent.
    assert(!test_compiles("struct Holder { n: int }\n"
                          "func Holder::take(h: box Holder): int { return h.n; }\n"
                          "func main(): int {\n"
                          "    let h: box Holder = new Holder;\n"
                          "    h.take();\n"
                          "    return h.n;\n"
                          "}\n"));
}

int main(void) {
    test_a_function_is_called_on_its_type();
    test_a_function_takes_its_declared_parameters();
    test_the_sugar_fills_parameter_zero();
    test_a_function_taking_nothing_is_not_reached_on_a_value();
    test_an_unknown_name_on_a_type_is_rejected();
    test_a_function_needs_a_type_its_module_declares();
    test_a_function_consumes_what_it_is_given();
    test_a_consuming_function_is_reached_through_a_value();

    printf("All owned function tests passed\n");
    return 0;
}
