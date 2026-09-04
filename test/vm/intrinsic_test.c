#include "support/run.h"

static void test_an_intrinsic_needs_no_bound_body() {
    assert(test_run_int("func f(): int {\n"
                        "    let xs: array<int, 3> = [1, 20, 300];\n"
                        "    return *xs.index(1);\n"
                        "}\n"
                        "let r: int = f();") == 20);
}

static void test_an_intrinsic_is_declared_by_the_core_library() {
    assert(!test_compiles_on_vm("impl<T, N: int> array<T, N> {\n"
                                "    intrinsic func mine(self: &Self): int;\n"
                                "}\n"));
}

static void test_an_intrinsic_the_compiler_does_not_lower_is_refused() {
    assert(!test_compiles_on_vm("struct Row { n: int }\n"
                                "impl Row {\n"
                                "    intrinsic func nothing(self: &Self): int;\n"
                                "}\n"));
}

int main(void) {
    test_an_intrinsic_needs_no_bound_body();
    test_an_intrinsic_is_declared_by_the_core_library();
    test_an_intrinsic_the_compiler_does_not_lower_is_refused();

    return 0;
}
