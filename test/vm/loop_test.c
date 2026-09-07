#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_a_clause_loop_scopes_its_variable_to_the_loop() {
    assert(!test_compiles("func f(): i32 {\n"
                          "    for let i: i32 = 0; i < 4; i = i + 1 { }\n"
                          "    return i;\n"
                          "}\n"));
}

static void test_a_non_bool_condition_is_rejected() {
    assert(!test_compiles("func f(): i32 { let x: i32 = 1; for x { } return 0; }\n"));
}

static void test_break_outside_a_loop_is_rejected() {
    assert(!test_compiles("func f(): i32 { break; }\n"));
    assert(!test_compiles("func f(): i32 { continue; }\n"));
    assert(!test_compiles("func f(): i32 { if true { break; } return 0; }\n"));
}

static void test_a_slot_moved_before_a_break_is_dead_after_the_loop() {
    assert(!test_compiles("struct Box { n: i32 }\n"
                          "func main(): i32 {\n"
                          "    let a: *Box = box Box { n: 0 };\n"
                          "    for let i = 0; i < 2; i = i + 1 {\n"
                          "        if i == 1 { let b = a; break; }\n"
                          "    }\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_a_transfer_in_a_nested_loop_is_refused() {
    assert(!test_compiles("struct Box { n: i32 }\n"
                          "func main(): i32 {\n"
                          "    let a: *Box = box Box { n: 0 };\n"
                          "    for let i = 0; i < 2; i = i + 1 {\n"
                          "        for let j = 0; j < 2; j = j + 1 {\n"
                          "            let b = a;\n"
                          "        }\n"
                          "    }\n"
                          "    return 0;\n"
                          "}\n"));
}

int main(void) {
    test_a_clause_loop_scopes_its_variable_to_the_loop();
    test_a_non_bool_condition_is_rejected();
    test_break_outside_a_loop_is_rejected();
    test_a_slot_moved_before_a_break_is_dead_after_the_loop();
    test_a_transfer_in_a_nested_loop_is_refused();

    printf("loop tests passed\n");
    return 0;
}
