// What 'for' iterates over and when it stops. The parser tests cover the
// syntax of the three forms; the rules that the condition must be a bool and
// that 'break' and 'continue' need an enclosing loop live here.
#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_a_condition_loop_runs_until_it_is_false() {
    assert(test_run_int("func f(): int {\n"
                        "    let i: int = 0;\n"
                        "    for i < 3 { i = i + 1; }\n"
                        "    return i;\n"
                        "}\n"
                        "let r: int = f();\n") == 3);
}

static void test_a_false_condition_skips_the_body() {
    assert(test_run_int("func f(): int {\n"
                        "    let i: int = 0;\n"
                        "    for false { i = 1; }\n"
                        "    return i;\n"
                        "}\n"
                        "let r: int = f();\n") == 0);
}

static void test_a_clause_loop_scopes_its_variable_to_the_loop() {
    assert(test_run_int("func f(): int {\n"
                        "    let total: int = 0;\n"
                        "    for let i: int = 0; i < 4; i = i + 1 { total = total + i; }\n"
                        "    return total;\n"
                        "}\n"
                        "let r: int = f();\n") == 6);

    assert(!test_compiles("func f(): int {\n"
                          "    for let i: int = 0; i < 4; i = i + 1 { }\n"
                          "    return i;\n"
                          "}\n"));
}

static void test_break_leaves_the_loop() {
    assert(test_run_int("func f(): int {\n"
                        "    let i: int = 0;\n"
                        "    for { i = i + 1; if i > 2 { break; } }\n"
                        "    return i;\n"
                        "}\n"
                        "let r: int = f();\n") == 3);
}

static void test_continue_starts_the_next_iteration() {
    assert(test_run_int("func f(): int {\n"
                        "    let total: int = 0;\n"
                        "    for let i: int = 0; i < 5; i = i + 1 {\n"
                        "        if i < 3 { continue; }\n"
                        "        total = total + i;\n"
                        "    }\n"
                        "    return total;\n"
                        "}\n"
                        "let r: int = f();\n") == 7);
}

static void test_a_non_bool_condition_is_rejected() {
    assert(!test_compiles("func f(): int { let x: int = 1; for x { } return 0; }\n"));
}

static void test_break_outside_a_loop_is_rejected() {
    assert(!test_compiles("func f(): int { break; }\n"));
    assert(!test_compiles("func f(): int { continue; }\n"));
    assert(!test_compiles("func f(): int { if true { break; } return 0; }\n"));
}

// 'break' is a way of arriving after the loop, so what it carries reaches the
// post-loop state: a slot moved before one is dead there, exactly as it is
// after an 'if' arm that moved it.
static void test_a_slot_moved_before_a_break_is_dead_after_the_loop() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: *Box = new Box;\n"
                          "    for let i = 0; i < 2; i = i + 1 {\n"
                          "        if i == 1 { let b = move a; break; }\n"
                          "    }\n"
                          "    return a.n;\n"
                          "}\n"));
}

int main(void) {
    test_a_condition_loop_runs_until_it_is_false();
    test_a_false_condition_skips_the_body();
    test_a_clause_loop_scopes_its_variable_to_the_loop();
    test_break_leaves_the_loop();
    test_continue_starts_the_next_iteration();
    test_a_non_bool_condition_is_rejected();
    test_break_outside_a_loop_is_rejected();
    test_a_slot_moved_before_a_break_is_dead_after_the_loop();

    printf("loop tests passed\n");
    return 0;
}
