#include "support/run.h"

#include <assert.h>
#include <stdbool.h>

static void test_an_interface_declares_type_parameters_its_signatures_name() {
    assert(test_compiles("interface Holder<T> {\n"
                         "    func get(self: &Self): T;\n"
                         "}\n"
                         "struct IntBox { n: int }\n"
                         "impl IntBox as Holder<int> {\n"
                         "    func get(self: &Self): int { return self.n; }\n"
                         "}\n"));
}

static void test_an_implementation_disagreeing_with_the_argument_is_refused() {
    assert(!test_compiles("interface Holder<T> {\n"
                          "    func get(self: &Self): T;\n"
                          "}\n"
                          "struct IntBox { n: int }\n"
                          "impl IntBox as Holder<bool> {\n"
                          "    func get(self: &Self): int { return self.n; }\n"
                          "}\n"));
}

static void test_a_generic_type_implements_at_its_own_parameter() {
    assert(test_compiles("interface Holder<T> {\n"
                         "    func get(self: &Self): T;\n"
                         "}\n"
                         "struct Box<T> { value: T }\n"
                         "impl<T> Box<T> as Holder<T> {\n"
                         "    func get(self: &Self): T { return self.value; }\n"
                         "}\n"));
}

static void test_an_interface_taking_arguments_is_named_with_them() {
    assert(!test_compiles("interface Holder<T> {\n"
                          "    func get(self: &Self): T;\n"
                          "}\n"
                          "struct IntBox { n: int }\n"
                          "impl IntBox as Holder {\n"
                          "    func get(self: &Self): int { return self.n; }\n"
                          "}\n"));

    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int;\n"
                          "}\n"
                          "struct Bag { n: int }\n"
                          "impl Bag as Countable<int> {\n"
                          "    func count(self: &Self): int { return self.n; }\n"
                          "}\n"));
}

static void test_a_bound_names_the_interfaces_arguments() {
    assert(test_compiles("interface Holder<T> {\n"
                         "    func get(self: &Self): T;\n"
                         "}\n"
                         "struct IntBox { n: int }\n"
                         "impl IntBox as Holder<int> {\n"
                         "    func get(self: &Self): int { return self.n; }\n"
                         "}\n"
                         "func read<H: Holder<int>>(h: &H): int { return h.get(); }\n"));
}

static void test_a_bounded_body_gets_the_argument_the_bound_names() {
    assert(!test_compiles("interface Holder<T> {\n"
                          "    func get(self: &Self): T;\n"
                          "}\n"
                          "func read<H: Holder<int>>(h: &H): bool { return h.get(); }\n"));
}

static void test_a_bounded_call_runs_on_an_implementor() {
    assert(test_run_int("interface Holder<T> {\n"
                        "    func get(self: &Self): T;\n"
                        "}\n"
                        "struct IntBox { n: int }\n"
                        "impl IntBox as Holder<int> {\n"
                        "    func get(self: &Self): int { return self.n; }\n"
                        "}\n"
                        "func read<H: Holder<int>>(h: &H): int { return h.get(); }\n"
                        "func main(): int {\n"
                        "    let b = IntBox { n: 7 };\n"
                        "    return read(b);\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

int main(void) {
    test_an_interface_declares_type_parameters_its_signatures_name();
    test_an_implementation_disagreeing_with_the_argument_is_refused();
    test_a_generic_type_implements_at_its_own_parameter();
    test_an_interface_taking_arguments_is_named_with_them();
    test_a_bound_names_the_interfaces_arguments();
    test_a_bounded_body_gets_the_argument_the_bound_names();
    test_a_bounded_call_runs_on_an_implementor();

    return 0;
}
