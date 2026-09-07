#include "support/run.h"

static void test_a_bound_at_another_element_type_is_refused() {
    assert(!test_compiles("func first<C: Index<bool>>(c: &C): bool { return *c.index(0); }\n"
                          "func f(): bool {\n"
                          "    let xs: array<i32, 2> = [1, 2];\n"
                          "    return first(xs);\n"
                          "}\n"));
}

static void test_the_interface_is_named_without_an_import() {
    assert(test_compiles_on_vm("struct Row { n: i32 }\n"
                               "impl Row as Index<i32> {\n"
                               "    func index(self: &Self, at: i32): &i32 { return self.n; }\n"
                               "}\n"));
}

static void test_an_array_supplies_index() {
    assert(test_run_int("func first<C: Index<i32>>(c: &C): i32 { return *c.index(0); }\n"
                        "func f(): i32 {\n"
                        "    let xs: array<i32, 3> = [1, 20, 300];\n"
                        "    return first(xs);\n"
                        "}\n"
                        "let r: i32 = f();") == 1);
}

static void test_an_array_is_read_with_brackets() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 3> = [1, 20, 300];\n"
                        "    return xs[2];\n"
                        "}\n"
                        "let r: i32 = f();") == 300);
}

static void test_brackets_reach_an_implementor_through_a_bound() {
    assert(test_run_int("func first<C: Index<i32>>(c: &C): i32 { return c[0]; }\n"
                        "func f(): i32 {\n"
                        "    let xs: array<i32, 2> = [9, 1];\n"
                        "    return first(xs);\n"
                        "}\n"
                        "let r: i32 = f();") == 9);
}

static void test_a_type_supplying_no_index_is_refused() {
    assert(!test_compiles_on_vm("struct Plain { n: i32 }\n"
                                "func f(): i32 {\n"
                                "    let p = Plain { n: 1 };\n"
                                "    return p[0];\n"
                                "}\n"));

    assert(test_diagnostic_mentions("struct Plain { n: i32 }\n"
                                    "func f(): i32 {\n"
                                    "    let p = Plain { n: 1 };\n"
                                    "    return p[0];\n"
                                    "}\n",
                                    "Index"));
}

static void test_an_element_is_written_through_brackets() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 2> = [1, 2];\n"
                        "    xs[0] = 42;\n"
                        "    return xs[0];\n"
                        "}\n"
                        "let r: i32 = f();") == 42);
}

static void test_an_index_that_lends_nothing_is_refused() {
    assert(test_diagnostic_mentions("struct Row { n: i32 }\n"
                                    "impl Row {\n"
                                    "    func index(self: &Self, at: i32): i32 { return self.n; }\n"
                                    "}\n"
                                    "func f(): i32 {\n"
                                    "    let r = Row { n: 1 };\n"
                                    "    return r[0];\n"
                                    "}\n",
                                    "lending"));
}

int main(void) {
    test_a_bound_at_another_element_type_is_refused();
    test_the_interface_is_named_without_an_import();
    test_an_array_supplies_index();
    test_an_array_is_read_with_brackets();
    test_brackets_reach_an_implementor_through_a_bound();
    test_a_type_supplying_no_index_is_refused();
    test_an_index_that_lends_nothing_is_refused();
    test_an_element_is_written_through_brackets();

    return 0;
}
