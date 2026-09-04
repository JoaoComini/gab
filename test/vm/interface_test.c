#include "ast/ast.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/run.h"
#include "support/test_context.h"
#include "type/type.h"

#include <assert.h>
#include <stdbool.h>

static void test_an_interface_declares_a_signature_its_implementors_supply() {
    assert(test_compiles("interface Countable {\n"
                         "    func count(self: &Self): int;\n"
                         "}\n"
                         "struct Bag { n: int }\n"
                         "impl Bag as Countable {\n"
                         "    func count(self: &Bag): int { return self.n; }\n"
                         "}\n"));
}

static void test_an_implementation_missing_a_method_is_refused() {
    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int;\n"
                          "}\n"
                          "struct Bag { n: int }\n"
                          "impl Bag as Countable {\n"
                          "}\n"));

    assert(test_diagnostic_mentions("interface Countable {\n"
                                    "    func count(self: &Self): int;\n"
                                    "}\n"
                                    "struct Bag { n: int }\n"
                                    "impl Bag as Countable {\n"
                                    "}\n",
                                    "count"));
}

static void test_a_method_whose_return_type_differs_is_refused() {
    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int;\n"
                          "}\n"
                          "struct Bag { n: int }\n"
                          "impl Bag as Countable {\n"
                          "    func count(self: &Bag): bool { return true; }\n"
                          "}\n"));
}

static void test_a_method_whose_parameters_differ_is_refused() {
    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int;\n"
                          "}\n"
                          "struct Bag { n: int }\n"
                          "impl Bag as Countable {\n"
                          "    func count(self: &Bag, extra: int): int { return extra; }\n"
                          "}\n"));
}

static void test_self_names_the_implementing_type() {
    assert(test_compiles("interface Sink {\n"
                         "    func absorb(self: &Self, other: &Self): int;\n"
                         "}\n"
                         "struct Bag { n: int }\n"
                         "impl Bag as Sink {\n"
                         "    func absorb(self: &Bag, other: &Bag): int { return self.n + other.n; }\n"
                         "}\n"));

    assert(!test_compiles("interface Sink {\n"
                          "    func absorb(self: &Self, other: &Self): int;\n"
                          "}\n"
                          "struct Bag { n: int }\n"
                          "struct Cup { n: int }\n"
                          "impl Bag as Sink {\n"
                          "    func absorb(self: &Bag, other: &Cup): int { return other.n; }\n"
                          "}\n"));
}

static void test_an_interface_is_named_before_it_is_declared() {
    assert(test_compiles("struct Bag { n: int }\n"
                         "impl Bag as Countable {\n"
                         "    func count(self: &Self): int { return self.n; }\n"
                         "}\n"
                         "interface Countable {\n"
                         "    func count(self: &Self): int;\n"
                         "}\n"));
}

static void test_naming_an_interface_that_is_not_declared_is_refused() {
    assert(!test_compiles("struct Bag { n: int }\n"
                          "impl Bag as Countable {\n"
                          "    func count(self: &Bag): int { return self.n; }\n"
                          "}\n"));
}

static void test_an_implemented_method_is_called_as_an_ordinary_method() {
    assert(test_run_int("interface Countable {\n"
                        "    func count(self: &Self): int;\n"
                        "}\n"
                        "struct Bag { n: int }\n"
                        "impl Bag as Countable {\n"
                        "    func count(self: &Bag): int { return self.n; }\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let b = Bag { n: 7 };\n"
                        "    return b.count();\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

static void test_a_generic_type_implements_an_interface() {
    assert(test_compiles("interface Countable {\n"
                         "    func count(self: &Self): int;\n"
                         "}\n"
                         "struct Holder<T> { value: T }\n"
                         "impl<T> Holder<T> as Countable {\n"
                         "    func count(self: &Holder<T>): int { return 1; }\n"
                         "}\n"));
}

static void test_an_interface_declares_no_body() {
    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int { return 0; }\n"
                          "}\n"));
}

static void test_a_type_implements_an_interface_once() {
    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int;\n"
                          "}\n"
                          "struct Bag { n: int }\n"
                          "impl Bag as Countable {\n"
                          "    func count(self: &Bag): int { return self.n; }\n"
                          "}\n"
                          "impl Bag as Countable {\n"
                          "}\n"));
}

static void test_implementing_a_generic_covers_its_instantiations() {
    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int;\n"
                          "}\n"
                          "struct Holder<T> { value: T }\n"
                          "impl<T> Holder<T> as Countable {\n"
                          "    func count(self: &Self): int { return 1; }\n"
                          "}\n"
                          "impl<T> Holder<T> as Countable {\n"
                          "}\n"));
}

static void test_an_impl_block_without_an_interface_still_declares_methods() {
    assert(test_run_int("struct Bag { n: int }\n"
                        "impl Bag {\n"
                        "    func count(self: &Bag): int { return self.n; }\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let b = Bag { n: 3 };\n"
                        "    return b.count();\n"
                        "}\n"
                        "let r: int = main();") == 3);
}

static void test_self_names_the_type_an_impl_block_is_for() {
    assert(test_run_int("struct Bag { n: int }\n"
                        "impl Bag {\n"
                        "    func count(self: &Self): int { return self.n; }\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let b = Bag { n: 4 };\n"
                        "    return b.count();\n"
                        "}\n"
                        "let r: int = main();") == 4);
}

static void test_a_generic_impl_spells_its_own_arguments_as_self() {
    assert(test_compiles("struct Holder<T> { value: T }\n"
                         "impl<T> Holder<T> {\n"
                         "    func get(self: &Self): T { return self.value; }\n"
                         "}\n"));
}

static void test_self_is_not_a_type_outside_an_impl_block() {
    assert(!test_compiles("func count(x: &Self): int { return 0; }\n"));
}

static void test_self_is_not_a_name_a_declaration_may_take() {
    assert(!test_compiles("struct Self { n: int }\n"));
    assert(!test_compiles("func Self(): int { return 0; }\n"));
    assert(!test_compiles("struct Holder<Self> { value: Self }\n"));
    assert(!test_compiles("interface Self {\n    func count(x: &int): int;\n}\n"));
    assert(!test_compiles("func f(): int { let Self: int = 1; return Self; }\n"));
}

static void test_self_outside_an_impl_block_says_so() {
    assert(test_diagnostic_mentions("func count(x: &Self): int { return 0; }\n", "impl"));
}

static void test_an_interfaces_method_is_supplied_in_the_block_that_implements_it() {
    assert(!test_compiles("interface Countable {\n"
                          "    func count(self: &Self): int;\n"
                          "}\n"
                          "struct Bag { n: int }\n"
                          "impl Bag {\n"
                          "    func count(self: &Self): int { return self.n; }\n"
                          "}\n"
                          "impl Bag as Countable {}\n"));

    assert(test_compiles("interface Countable {\n"
                         "    func count(self: &Self): int;\n"
                         "}\n"
                         "struct Bag { n: int }\n"
                         "impl Bag as Countable {\n"
                         "    func count(self: &Self): int { return self.n; }\n"
                         "}\n"));
}

int main(void) {
    test_an_interface_declares_a_signature_its_implementors_supply();
    test_an_implementation_missing_a_method_is_refused();
    test_a_method_whose_return_type_differs_is_refused();
    test_a_method_whose_parameters_differ_is_refused();
    test_self_names_the_implementing_type();
    test_an_interface_is_named_before_it_is_declared();
    test_naming_an_interface_that_is_not_declared_is_refused();
    test_an_implemented_method_is_called_as_an_ordinary_method();
    test_a_generic_type_implements_an_interface();
    test_an_interface_declares_no_body();
    test_a_type_implements_an_interface_once();
    test_implementing_a_generic_covers_its_instantiations();
    test_an_impl_block_without_an_interface_still_declares_methods();
    test_self_names_the_type_an_impl_block_is_for();
    test_a_generic_impl_spells_its_own_arguments_as_self();
    test_self_is_not_a_type_outside_an_impl_block();
    test_self_is_not_a_name_a_declaration_may_take();
    test_self_outside_an_impl_block_says_so();
    test_an_interfaces_method_is_supplied_in_the_block_that_implements_it();

    return 0;
}
