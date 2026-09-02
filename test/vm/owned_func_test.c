#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void test_a_function_is_called_on_its_type() {
    assert(test_run_int("struct Counter { n: int }\n"
                        "impl Counter {\n"
                        "    func start(): int { return 7; }\n"
                        "}\n"
                        "func main(): int { return Counter::start(); }\n"
                        "let r: int = main();") == 7);
}

static void test_a_function_takes_its_declared_parameters() {
    assert(test_run_int("struct Counter { n: int }\n"
                        "impl Counter {\n"
                        "    func of(a: int, b: int): int { return a + b; }\n"
                        "}\n"
                        "func main(): int { return Counter::of(3, 4); }\n"
                        "let r: int = main();") == 7);
}

static void test_the_sugar_fills_parameter_zero() {
    assert(test_run_int("struct Counter { n: int }\n"
                        "impl Counter {\n"
                        "    func of(c: &Counter): int { return c.n; }\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let c = Counter { n: 5 };\n"
                        "    return c.of() + Counter::of(c);\n"
                        "}\n"
                        "let r: int = main();") == 10);
}

static void test_a_function_taking_nothing_is_not_reached_on_a_value() {
    assert(!test_compiles("struct Counter { n: int }\n"
                          "impl Counter {\n"
                          "    func start(): int { return 7; }\n"
                          "}\n"
                          "func main(): int { let c: Counter; return c.start(); }\n"));
}

static void test_an_unknown_name_on_a_type_is_rejected() {
    assert(!test_compiles("struct Counter { n: int }\n"
                          "func main(): int { return Counter::missing(); }\n"));
}

static void test_a_function_needs_a_type_its_module_declares() {
    assert(!test_compiles("impl Missing {\n"
                          "    func start(): int { return 7; }\n"
                          "}\n"));
}

static void test_a_function_consumes_what_it_is_given() {
    assert(test_run_int("struct Holder { n: int }\n"
                        "impl Holder {\n"
                        "    func take(h: *Holder): int { return h.n; }\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let h: *Holder = box Holder { n: 0 };\n"
                        "    h.n = 6;\n"
                        "    return Holder::take(h);\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

static void test_a_consuming_function_is_reached_through_a_value() {
    assert(test_run_int("struct Holder { n: int }\n"
                        "impl Holder {\n"
                        "    func take(h: *Holder): int { return h.n; }\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let h: *Holder = box Holder { n: 0 };\n"
                        "    h.n = 4;\n"
                        "    return h.take();\n"
                        "}\n"
                        "let r: int = main();") == 4);

    assert(!test_compiles("struct Holder { n: int }\n"
                          "impl Holder {\n"
                          "    func take(h: *Holder): int { return h.n; }\n"
                          "}\n"
                          "func main(): int {\n"
                          "    let h: *Holder = box Holder { n: 0 };\n"
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
