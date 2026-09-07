#include "support/run.h"

static void test_an_intrinsic_needs_no_bound_body() {
    assert(test_run_int("func f(): i32 {\n"
                        "    let xs: array<i32, 3> = [1, 20, 300];\n"
                        "    return *xs.index(1);\n"
                        "}\n"
                        "let r: i32 = f();") == 20);
}

static void test_an_intrinsic_is_declared_by_the_core_library() {
    assert(!test_compiles_on_vm("impl<T, N: i32> array<T, N> {\n"
                                "    intrinsic func mine(self: &Self): i32;\n"
                                "}\n"));
}

static void test_an_intrinsic_the_compiler_does_not_lower_is_refused() {
    assert(!test_compiles_on_vm("struct Row { n: i32 }\n"
                                "impl Row {\n"
                                "    intrinsic func nothing(self: &Self): i32;\n"
                                "}\n"));
}

static void test_an_intrinsic_names_a_lowering_the_compiler_has() {
    assert(!test_compiles_as_prelude("impl str {\n"
                                     "    intrinsic func nowhere(self: &str): i32;\n"
                                     "}\n"));
}

int main(void) {
    test_an_intrinsic_needs_no_bound_body();
    test_an_intrinsic_is_declared_by_the_core_library();
    test_an_intrinsic_the_compiler_does_not_lower_is_refused();
    test_an_intrinsic_names_a_lowering_the_compiler_has();

    return 0;
}
