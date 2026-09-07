#include "support/run.h"

#include <assert.h>
#include <stdbool.h>

static void test_a_method_declares_its_own_type_parameter() {
    assert(test_compiles("struct Bag { n: i32 }\n"
                         "impl Bag {\n"
                         "    func pick<U>(self: &Bag, other: U): U { return other; }\n"
                         "}\n"));
}

static void test_a_method_type_parameter_is_inferred_from_the_argument() {
    assert(test_run_int("struct Bag { n: i32 }\n"
                        "impl Bag {\n"
                        "    func pick<U>(self: &Bag, other: U): U { return other; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let b = Bag { n: 1 };\n"
                        "    return b.pick(9);\n"
                        "}\n"
                        "let r: i32 = main();") == 9);
}

static void test_one_method_serves_each_type_it_is_called_with() {
    assert(test_run_float("struct Bag { n: i32 }\n"
                          "impl Bag {\n"
                          "    func pick<U>(self: &Bag, other: U): U { return other; }\n"
                          "}\n"
                          "func main(): f32 {\n"
                          "    let b = Bag { n: 1 };\n"
                          "    let i: i32 = b.pick(3);\n"
                          "    return b.pick(2.5);\n"
                          "}\n"
                          "let r: f32 = main();") == 2.5f);
}

static void test_a_method_on_a_generic_type_declares_its_own_parameter() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "impl<T> Holder<T> {\n"
                        "    func pick<U>(self: &Holder<T>, other: U): U { return other; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let h = Holder<i32> { value: 1 };\n"
                        "    return h.pick(7);\n"
                        "}\n"
                        "let r: i32 = main();") == 7);
}

static void test_a_method_reaches_both_its_own_parameter_and_its_owners() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "impl<T> Holder<T> {\n"
                        "    func swap<U>(self: &Holder<T>, other: U): T { return self.value; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let h = Holder<i32> { value: 5 };\n"
                        "    return h.swap(2.5);\n"
                        "}\n"
                        "let r: i32 = main();") == 5);
}

static void test_a_miscounted_call_names_the_count_rather_than_inference() {
    assert(test_diagnostic_mentions("struct Bag { n: i32 }\n"
                                    "impl Bag {\n"
                                    "    func pick<U>(self: &Bag, other: U): U { return other; }\n"
                                    "}\n"
                                    "func main(): i32 {\n"
                                    "    let b = Bag { n: 1 };\n"
                                    "    return b.pick();\n"
                                    "}\n",
                                    "expected 1 argument(s), found 0"));
}

static void test_a_generic_free_call_names_its_count_too() {
    assert(test_diagnostic_mentions("func pick<U>(a: U, b: U): U { return a; }\n"
                                    "func main(): i32 { return pick(1); }\n",
                                    "expected 2 argument(s), found 1"));
}

static void test_a_written_type_argument_still_counts_its_arguments() {
    assert(test_diagnostic_mentions("func pick<U>(a: U, b: U): U { return a; }\n"
                                    "func main(): i32 { return pick<i32>(1); }\n",
                                    "expected 2 argument(s), found 1"));
}

static void test_a_parameter_no_argument_names_is_refused() {
    assert(!test_compiles("struct Bag { n: i32 }\n"
                          "impl Bag {\n"
                          "    func make<U>(self: &Bag): U;\n"
                          "}\n"));
}

static void test_a_method_shadowing_its_owners_parameter_is_refused() {
    assert(!test_compiles("struct Holder<T> { value: T }\n"
                          "impl<T> Holder<T> {\n"
                          "    func pick<T>(self: &Holder<T>, other: T): T { return other; }\n"
                          "}\n"));
}

int main(void) {
    test_a_method_declares_its_own_type_parameter();
    test_a_method_type_parameter_is_inferred_from_the_argument();
    test_one_method_serves_each_type_it_is_called_with();
    test_a_method_on_a_generic_type_declares_its_own_parameter();
    test_a_method_reaches_both_its_own_parameter_and_its_owners();
    test_a_miscounted_call_names_the_count_rather_than_inference();
    test_a_generic_free_call_names_its_count_too();
    test_a_written_type_argument_still_counts_its_arguments();
    test_a_parameter_no_argument_names_is_refused();
    test_a_method_shadowing_its_owners_parameter_is_refused();

    return 0;
}
