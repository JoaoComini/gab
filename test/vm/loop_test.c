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

static void test_a_slot_moved_before_a_break_is_dead_after_the_loop() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: *Box = box Box { n: 0 };\n"
                          "    for let i = 0; i < 2; i = i + 1 {\n"
                          "        if i == 1 { let b = a; break; }\n"
                          "    }\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_a_transfer_in_a_nested_loop_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let a: *Box = box Box { n: 0 };\n"
                          "    for let i = 0; i < 2; i = i + 1 {\n"
                          "        for let j = 0; j < 2; j = j + 1 {\n"
                          "            let b = a;\n"
                          "        }\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

static void test_both_spellings_of_a_step_count_alike() {
    assert(test_run_int("func run(n: int): int {\n"
                        "    let acc: int = 0;\n"
                        "    for let i: int = 0; i < n; i = i + 1 { acc = acc + i; }\n"
                        "    return acc;\n"
                        "}\n"
                        "let r: int = run(5);\n") == 10);

    assert(test_run_int("func run(n: int): int {\n"
                        "    let acc: int = 0;\n"
                        "    for let i: int = 0; i < n; i += 1 { acc = acc + i; }\n"
                        "    return acc;\n"
                        "}\n"
                        "let r: int = run(5);\n") == 10);

    assert(test_run_int("func run(n: int): int {\n"
                        "    let acc: int = 0;\n"
                        "    for let i: int = 0; i < n; i = 1 + i { acc = acc + i; }\n"
                        "    return acc;\n"
                        "}\n"
                        "let r: int = run(5);\n") == 10);
}

static void test_a_body_that_steps_the_counter_still_runs_as_written() {
    assert(test_run_int("func run(n: int): int {\n"
                        "    let acc: int = 0;\n"
                        "    for let i: int = 0; i < n; i = i + 1 { acc = acc + 1; i = i + 1; }\n"
                        "    return acc;\n"
                        "}\n"
                        "let r: int = run(6);\n") == 3);
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
    test_a_transfer_in_a_nested_loop_is_refused();

    test_both_spellings_of_a_step_count_alike();
    test_a_body_that_steps_the_counter_still_runs_as_written();

    printf("loop tests passed\n");
    return 0;
}
