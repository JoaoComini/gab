#include "support/run.h"

static void test_a_vec_indexes_at_its_element_type() {
    assert(test_run_int("import std;\n"
                        "func first<C: Index<int>>(c: &C): int { return *c.index(0); }\n"
                        "func f(): int {\n"
                        "    let xs: Vec<int> = Vec<int>::new(0);\n"
                        "    xs.push(7);\n"
                        "    return first(xs);\n"
                        "}\n"
                        "let r: int = f();") == 7);
}

static void test_a_bound_at_another_element_type_is_refused() {
    assert(!test_compiles_on_vm("import std;\n"
                                "func first<C: Index<bool>>(c: &C): bool { return *c.index(0); }\n"
                                "func f(): bool {\n"
                                "    let xs: Vec<int> = Vec<int>::new(0);\n"
                                "    xs.push(7);\n"
                                "    return first(xs);\n"
                                "}\n"));
}

static void test_the_interface_is_named_without_an_import() {
    assert(test_compiles_on_vm("struct Row { n: int }\n"
                               "impl Row as Index<int> {\n"
                               "    func index(self: &Self, at: int): &int { return self.n; }\n"
                               "}\n"));
}

static void test_an_array_supplies_index() {
    assert(test_run_int("func first<C: Index<int>>(c: &C): int { return *c.index(0); }\n"
                        "func f(): int {\n"
                        "    let xs: [int; 3] = [1, 20, 300];\n"
                        "    return first(xs);\n"
                        "}\n"
                        "let r: int = f();") == 1);
}

static void test_an_array_is_read_with_brackets() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: [int; 3] = [1, 20, 300];\n"
                        "    return xs[2];\n"
                        "}\n"
                        "let r: int = f();") == 300);
}

static void test_a_vec_is_read_with_brackets() {
    assert(test_run_int("import std;\n"
                        "func f(): int {\n"
                        "    let xs: Vec<int> = Vec<int>::new(0);\n"
                        "    xs.push(7);\n"
                        "    return xs[0];\n"
                        "}\n"
                        "let r: int = f();") == 7);
}

static void test_brackets_reach_an_implementor_through_a_bound() {
    assert(test_run_int("import std;\n"
                        "func first<C: Index<int>>(c: &C): int { return c[0]; }\n"
                        "func f(): int {\n"
                        "    let xs: Vec<int> = Vec<int>::new(0);\n"
                        "    xs.push(9);\n"
                        "    return first(xs);\n"
                        "}\n"
                        "let r: int = f();") == 9);
}

static void test_a_type_supplying_no_index_is_refused() {
    assert(!test_compiles_on_vm("struct Plain { n: int }\n"
                                "func f(): int {\n"
                                "    let p = Plain { n: 1 };\n"
                                "    return p[0];\n"
                                "}\n"));

    assert(test_diagnostic_mentions("struct Plain { n: int }\n"
                                    "func f(): int {\n"
                                    "    let p = Plain { n: 1 };\n"
                                    "    return p[0];\n"
                                    "}\n",
                                    "Index"));
}

static void test_an_element_is_written_through_brackets() {
    assert(test_run_int("import std;\n"
                        "func f(): int {\n"
                        "    let xs: Vec<int> = Vec<int>::new(0);\n"
                        "    xs.push(1);\n"
                        "    xs[0] = 42;\n"
                        "    return xs[0];\n"
                        "}\n"
                        "let r: int = f();") == 42);
}

static void test_an_index_that_lends_nothing_is_refused() {
    assert(test_diagnostic_mentions("struct Row { n: int }\n"
                                    "impl Row {\n"
                                    "    func index(self: &Self, at: int): int { return self.n; }\n"
                                    "}\n"
                                    "func f(): int {\n"
                                    "    let r = Row { n: 1 };\n"
                                    "    return r[0];\n"
                                    "}\n",
                                    "lending"));
}

int main(void) {
    test_a_vec_indexes_at_its_element_type();
    test_a_bound_at_another_element_type_is_refused();
    test_the_interface_is_named_without_an_import();
    test_an_array_supplies_index();
    test_an_array_is_read_with_brackets();
    test_a_vec_is_read_with_brackets();
    test_brackets_reach_an_implementor_through_a_bound();
    test_a_type_supplying_no_index_is_refused();
    test_an_index_that_lends_nothing_is_refused();
    test_an_element_is_written_through_brackets();

    return 0;
}
