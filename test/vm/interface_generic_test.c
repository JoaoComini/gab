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

static void test_an_impl_block_bounds_its_own_type_parameter() {
    assert(test_compiles("interface Countable {\n"
                         "    func count(self: &Self): int;\n"
                         "}\n"
                         "struct Pair<T> { a: T, b: T }\n"
                         "impl<T: Countable> Pair<T> {\n"
                         "    func total(self: &Self): int { return self.a.count(); }\n"
                         "}\n"));
}

static void test_an_impl_block_body_calling_what_no_bound_declares_is_refused() {
    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int;\n"
                          "}\n"
                          "struct Pair<T> { a: T, b: T }\n"
                          "impl<T: Countable> Pair<T> {\n"
                          "    func total(self: &Self): int { return self.a.missing(); }\n"
                          "}\n"));
}

static void test_an_unbounded_impl_parameter_has_no_methods() {
    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int;\n"
                          "}\n"
                          "struct Pair<T> { a: T, b: T }\n"
                          "impl<T> Pair<T> {\n"
                          "    func total(self: &Self): int { return self.a.count(); }\n"
                          "}\n"));
}

static void test_an_impl_method_whose_result_is_concrete_still_instantiates() {
    assert(test_run_int("struct Holder<T> { value: T }\n"
                        "impl<T> Holder<T> {\n"
                        "    func size(h: &Holder<T>): int { return 4; }\n"
                        "}\n"
                        "struct Wrap<T> { inner: Holder<T> }\n"
                        "impl<T> Wrap<T> {\n"
                        "    func how_big(w: &Wrap<T>): int { return w.inner.size(); }\n"
                        "}\n"
                        "func f(): int { let w = Wrap<int> { inner: Holder<int> { value: 6 } };\n"
                        "return w.how_big(); }\n"
                        "let r: int = f();") == 4);
}

int main(void) {
    test_an_interface_declares_type_parameters_its_signatures_name();
    test_an_implementation_disagreeing_with_the_argument_is_refused();
    test_a_generic_type_implements_at_its_own_parameter();
    test_an_interface_taking_arguments_is_named_with_them();
    test_a_bound_names_the_interfaces_arguments();
    test_a_bounded_body_gets_the_argument_the_bound_names();
    test_a_bounded_call_runs_on_an_implementor();
    test_an_impl_block_bounds_its_own_type_parameter();
    test_an_impl_block_body_calling_what_no_bound_declares_is_refused();
    test_an_unbounded_impl_parameter_has_no_methods();
    test_an_impl_method_whose_result_is_concrete_still_instantiates();

    return 0;
}
