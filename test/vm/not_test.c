#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_negates_a_literal() {
    assert(test_run_bool("func f(): bool { return !true; }\n"
                         "let r: bool = f();\n") == false);
}

static void test_negates_a_variable() {
    assert(test_run_bool("func f(): bool { let b: bool = false; return !b; }\n"
                         "let r: bool = f();\n") == true);
}

static void test_negates_a_comparison() {
    assert(test_run_bool("func f(): bool { let x: int = 1; return !(x < 2); }\n"
                         "let r: bool = f();\n") == false);
}

static void test_negates_a_call_result() {
    assert(test_run_bool("func yes(): bool { return true; }\n"
                         "func f(): bool { return !yes(); }\n"
                         "let r: bool = f();\n") == false);
}

static void test_negates_through_a_deref() {
    assert(test_run_bool("func f(): bool { let b: bool = true; let p: ref bool = b; return !*p; }\n"
                         "let r: bool = f();\n") == false);
}

static void test_double_negation_cancels() {
    assert(test_run_bool("func f(): bool { let b: bool = true; return !!b; }\n"
                         "let r: bool = f();\n") == true);
}

static void test_binds_tighter_than_a_binary_operator() {
    assert(test_run_bool("func f(): bool { let a: bool = false; let b: bool = false; return !a && b; }\n"
                         "let r: bool = f();\n") == false);
}

static void test_binds_looser_than_a_postfix() {
    assert(test_run_bool("struct Flags { on: bool }\n"
                         "func f(): bool { let g = Flags { on: false }; return !g.on; }\n"
                         "let r: bool = f();\n") == true);
}

static void test_not_is_typed_boolean() {
    assert(test_compiles("func f(): bool { let b: bool = true; return !b; }\n"));

    assert(!test_compiles("func f(): bool { let x: int = 1; return !x; }\n"));
    assert(!test_compiles("func f(): bool { let x: float = 1.0; return !x; }\n"));
    assert(!test_compiles("func f(): bool { let x: int = 1; let p: ref int = x; return !p; }\n"));
}

static void test_not_is_a_temporary() {
    assert(!test_compiles("func f(): bool { let b: bool = true; !b = false; return b; }\n"));
    assert(!test_compiles("func f(): bool { let b: bool = true; let p: ref bool = !b; return *p; }\n"));
}

int main() {
    test_negates_a_literal();
    test_negates_a_variable();
    test_negates_a_comparison();
    test_negates_a_call_result();
    test_negates_through_a_deref();
    test_double_negation_cancels();
    test_binds_tighter_than_a_binary_operator();
    test_binds_looser_than_a_postfix();
    test_not_is_typed_boolean();
    test_not_is_a_temporary();

    printf("not_test: all tests passed\n");
    return 0;
}
