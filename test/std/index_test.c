#include "support/run.h"

static void test_a_vec_indexes_at_its_element_type() {
    assert(test_run_int("import std;\n"
                        "func first<C: Index<int>>(c: &C): int { return *c.at(0); }\n"
                        "func f(): int {\n"
                        "    let xs: Vec<int> = Vec<int>::new(0);\n"
                        "    xs.push(7);\n"
                        "    return first(xs);\n"
                        "}\n"
                        "let r: int = f();") == 7);
}

static void test_a_vec_iterates_at_its_element_type() {
    assert(test_run_int("import std;\n"
                        "func total<C: Iter<int>>(c: &C): int {\n"
                        "    let sum: int = 0;\n"
                        "    for let i: int = 0; i < c.len(); i = i + 1 { sum = sum + *c.at(i); }\n"
                        "    return sum;\n"
                        "}\n"
                        "func f(): int {\n"
                        "    let xs: Vec<int> = Vec<int>::new(0);\n"
                        "    xs.push(1);\n"
                        "    xs.push(20);\n"
                        "    xs.push(300);\n"
                        "    return total(xs);\n"
                        "}\n"
                        "let r: int = f();") == 321);
}

static void test_a_bound_at_another_element_type_is_refused() {
    assert(!test_compiles_on_vm("import std;\n"
                                "func first<C: Index<bool>>(c: &C): bool { return *c.at(0); }\n"
                                "func f(): bool {\n"
                                "    let xs: Vec<int> = Vec<int>::new(0);\n"
                                "    xs.push(7);\n"
                                "    return first(xs);\n"
                                "}\n"));
}

static void test_the_interfaces_are_named_without_an_import() {
    assert(test_compiles_on_vm("struct Row { n: int }\n"
                               "impl Row as Index<int> {\n"
                               "    func at(self: &Self, index: int): &int { return self.n; }\n"
                               "}\n"));
}

static void test_an_array_supplies_the_interfaces() {
    assert(test_run_int("func total<C: Iter<int>>(c: &C): int {\n"
                        "    let sum: int = 0;\n"
                        "    for let i: int = 0; i < c.len(); i = i + 1 { sum = sum + *c.at(i); }\n"
                        "    return sum;\n"
                        "}\n"
                        "func f(): int {\n"
                        "    let xs: [int; 3] = [1, 20, 300];\n"
                        "    return total(xs);\n"
                        "}\n"
                        "let r: int = f();") == 321);
}

int main(void) {
    test_a_vec_indexes_at_its_element_type();
    test_a_vec_iterates_at_its_element_type();
    test_a_bound_at_another_element_type_is_refused();
    test_the_interfaces_are_named_without_an_import();
    test_an_array_supplies_the_interfaces();

    return 0;
}
