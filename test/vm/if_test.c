// What 'if' requires of its condition, and that it branches on it correctly.
// The parser tests cover the syntax; the rule that the condition must be a
// bool is a resolver rule, and lives here.
#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_branches_on_a_bool() {
    assert(test_run_int("func f(): int { if true { return 1; } return 0; }\n"
                        "let r: int = f();\n") == 1);

    assert(test_run_int("func f(): int { if false { return 1; } return 0; }\n"
                        "let r: int = f();\n") == 0);
}

static void test_else_runs_when_the_condition_is_false() {
    assert(test_run_int("func f(): int { if false { return 1; } else { return 2; } }\n"
                        "let r: int = f();\n") == 2);
}

static void test_a_comparison_is_a_valid_condition() {
    assert(test_compiles("func f(): int { let x: int = 1; if x > 0 { return 1; } return 0; }\n"));
    assert(test_compiles("func f(): int { let x: bool = true; if x { return 1; } return 0; }\n"));
    assert(test_compiles("func f(): int { if true && false { return 1; } return 0; }\n"));
}

static void test_a_non_bool_condition_is_rejected() {
    assert(!test_compiles("func f(): int { let x: int = 1; if x { return 1; } return 0; }\n"));

    assert(!test_compiles("func f(): float { let x: float = 1.0; if x { return 1.0; } return 0.0; }\n"));

    assert(!test_compiles(
        "func f(): int { let x: int = 1; let p: ref int = ref x; if p { return 1; } return 0; }\n"));

    assert(!test_compiles("struct V { x: int }\n"
                          "func f(): int { let v: V; if v { return 1; } return 0; }\n"));
}

// A condition that is already an error must not produce a second complaint
// about not being a bool: one mistake should report once.
static void test_a_poisoned_condition_reports_once() {
    assert(!test_compiles("func f(): int { if undefined_name { return 1; } return 0; }\n"));
}

int main() {
    test_branches_on_a_bool();
    test_else_runs_when_the_condition_is_false();
    test_a_comparison_is_a_valid_condition();
    test_a_non_bool_condition_is_rejected();
    test_a_poisoned_condition_reports_once();

    printf("if_test: all tests passed\n");
    return 0;
}
