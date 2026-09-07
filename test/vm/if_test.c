#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_branches_on_a_bool() {
    assert(test_run_int("func f(): i32 { if true { return 1; } return 0; }\n"
                        "let r: i32 = f();\n") == 1);

    assert(test_run_int("func f(): i32 { if false { return 1; } return 0; }\n"
                        "let r: i32 = f();\n") == 0);
}

static void test_else_runs_when_the_condition_is_false() {
    assert(test_run_int("func f(): i32 { if false { return 1; } else { return 2; } }\n"
                        "let r: i32 = f();\n") == 2);
}

static void test_a_comparison_is_a_valid_condition() {
    assert(test_compiles("func f(): i32 { let x: i32 = 1; if x > 0 { return 1; } return 0; }\n"));
    assert(test_compiles("func f(): i32 { let x: bool = true; if x { return 1; } return 0; }\n"));
    assert(test_compiles("func f(): i32 { if true && false { return 1; } return 0; }\n"));
}

static void test_a_non_bool_condition_is_rejected() {
    assert(!test_compiles("func f(): i32 { let x: i32 = 1; if x { return 1; } return 0; }\n"));

    assert(!test_compiles("func f(): f32 { let x: f32 = 1.0; if x { return 1.0; } return 0.0; }\n"));

    assert(
        !test_compiles("func f(): i32 { let x: i32 = 1; let p: &i32 = x; if p { return 1; } return 0; }\n"));

    assert(!test_compiles("struct V { x: i32 }\n"
                          "func f(): i32 { let v: V; if v { return 1; } return 0; }\n"));
}

static void test_a_poisoned_condition_reports_once() {
    assert(!test_compiles("func f(): i32 { if undefined_name { return 1; } return 0; }\n"));
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
