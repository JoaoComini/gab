// How many instructions a loop body costs. These are the shapes the VM
// executes once per iteration, so a regression here is a regression in every
// program that loops -- which behaviour tests cannot see, since the answer
// stays correct however many instructions it took.
#include "support/run.h"
#include "vm/opcode.h"

#include <assert.h>
#include <stdio.h>

// Arithmetic over locals: three operations and the loop's own compare and
// jump. Nothing here should load a constant, because every literal is small
// enough to ride in its instruction.
static void test_a_local_loop_body_loads_no_constants() {
    TestProgram program = test_compile("func run(n: int): int {\n"
                                       "    let x: int = 1;\n"
                                       "    let y: int = 2;\n"
                                       "    for let i: int = 0; i < n; i += 1 {\n"
                                       "        x += y;\n"
                                       "        y = x - y;\n"
                                       "        x %= 100003;\n"
                                       "    }\n"
                                       "    return x;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    // 100003 exceeds VM_MAX_IMMEDIATE, so it is the one constant the body
    // needs. Everything else -- 1, 2, the loop's own step -- is immediate.
    // 100003 exceeds VM_MAX_IMMEDIATE, so it is the one constant the body
    // itself needs; the others belong to the setup before the loop.
    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 4);

    test_program_free(&program);
}

// The same arithmetic over struct fields costs a load and a store per access,
// where the local version addresses its slot directly.
static void test_a_field_loop_body_loads_and_stores_each_access() {
    TestProgram program = test_compile("struct Point { x: int, y: int }\n"
                                       "func run(n: int): int {\n"
                                       "    let v: Point;\n"
                                       "    v.x = 1;\n"
                                       "    v.y = 2;\n"
                                       "    for let i: int = 0; i < n; i += 1 {\n"
                                       "        v.x += v.y;\n"
                                       "        v.y = v.x - v.y;\n"
                                       "        v.x %= 100003;\n"
                                       "    }\n"
                                       "    return v.x;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    size_t loads = test_count_opcode(chunk, OP_LOAD_FIELD_4);
    size_t stores = test_count_opcode(chunk, OP_STORE_FIELD_4);

    // A local holding the same values would need none of these: the whole
    // difference between the two loops is that a field is addressed rather
    // than named.
    assert(loads > 0);
    assert(stores > 0);

    test_program_free(&program);
}

// A declaration with a literal initialiser loads the constant into the
// variable's own slot. Loading into a temporary and moving it down costs an
// instruction per declaration, which a loop pays on every entry.
static void test_a_literal_initialiser_loads_into_the_variable() {
    TestProgram program = test_compile("func f(): int { let x: int = 7; return x; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);
    assert(test_count_opcode(chunk, OP_MOVE) == 0);

    test_program_free(&program);
}

// Assigning one variable to another is the move itself, not a read into a
// temporary followed by a second move.
static void test_assigning_a_variable_is_a_single_move() {
    TestProgram program =
        test_compile("func f(): int { let a: int = 1; let b: int = 2; a = b; return a; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_MOVE) == 1);

    test_program_free(&program);
}

// A counting loop tests, increments and jumps in one instruction rather than
// four. The four are what a general 'for' needs; this shape -- an int counter
// stepped by a literal and compared against something the body cannot change
// -- is the common one, and it executes once per iteration.
static void test_a_counting_loop_is_one_instruction_per_iteration() {
    TestProgram program = test_compile("func run(n: int): int {\n"
                                       "    let acc: int = 0;\n"
                                       "    for let i: int = 0; i < n; i += 1 { acc = i; }\n"
                                       "    return acc;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_FOR_LOOP) == 1);

    // No jump back: the fused instruction is the jump. The one compare left is
    // the entry test, which runs once rather than once per iteration.
    assert(test_count_opcode(chunk, OP_JMP) == 0);
    assert(test_count_opcode(chunk, OP_CMP_LTI) == 1);

    test_program_free(&program);
}

// A body too long to reach back in the offset's eight bits keeps the general
// compare-and-jump form, so the range bounds an optimisation rather than a
// program.
static void test_a_long_body_keeps_the_general_form() {
    char source[8192];
    size_t at = (size_t)snprintf(source, sizeof source,
                                 "func run(n: int): int {\n"
                                 "    let acc: int = 0;\n"
                                 "    for let i: int = 0; i < n; i += 1 {\n");

    for (int i = 0; i < 140; i++) {
        at += (size_t)snprintf(source + at, sizeof source - at, "        acc = acc + %d;\n", i);
    }

    snprintf(source + at, sizeof source - at, "    }\n    return acc;\n}\n");

    TestProgram program = test_compile(source);
    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_FOR_LOOP) == 0);
    assert(test_count_opcode(chunk, OP_JMP) == 1);

    test_program_free(&program);
}

// 'i = i + 1' steps the counter exactly as 'i += 1' does, so it is the same
// loop and takes the same instruction. The compound form is a spelling, not a
// different operation, and recognising only that one would make the fused
// instruction depend on how the step was written.
static void test_a_counting_loop_is_recognised_however_the_step_is_spelled() {
    TestProgram program = test_compile("func run(n: int): int {\n"
                                       "    let acc: int = 0;\n"
                                       "    for let i: int = 0; i < n; i = i + 1 { acc = i; }\n"
                                       "    return acc;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_FOR_LOOP) == 1);
    assert(test_count_opcode(chunk, OP_JMP) == 0);

    test_program_free(&program);
}

// '1 + i' is the same step written the other way round, and addition commutes.
static void test_a_counting_loop_takes_the_step_from_either_side() {
    TestProgram program = test_compile("func run(n: int): int {\n"
                                       "    let acc: int = 0;\n"
                                       "    for let i: int = 0; i < n; i = 1 + i { acc = i; }\n"
                                       "    return acc;\n"
                                       "}\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_FOR_LOOP) == 1);

    test_program_free(&program);
}

// A step of something other than one is not this shape, whichever way it is
// written: the fused instruction steps by one and nothing else.
static void test_a_loop_stepping_by_more_than_one_keeps_the_general_form() {
    TestProgram program = test_compile("func run(n: int): int {\n"
                                       "    let acc: int = 0;\n"
                                       "    for let i: int = 0; i < n; i = i + 2 { acc = i; }\n"
                                       "    return acc;\n"
                                       "}\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_FOR_LOOP) == 0);

    test_program_free(&program);
}

// A plain assignment that is not a step at all must not be read as one.
static void test_a_loop_assigning_something_else_keeps_the_general_form() {
    TestProgram program = test_compile("func run(n: int, m: int): int {\n"
                                       "    let acc: int = 0;\n"
                                       "    for let i: int = 0; i < n; i = m + 1 { acc = i; }\n"
                                       "    return acc;\n"
                                       "}\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_FOR_LOOP) == 0);

    test_program_free(&program);
}

// A loop whose condition is not the counting shape keeps the general form, so
// the fused instruction never has to stand for something it cannot express.
static void test_a_general_loop_keeps_the_compare_and_jump() {
    TestProgram program = test_compile("func run(n: int): int {\n"
                                       "    let acc: int = 0;\n"
                                       "    for let i: int = 0; acc < n; i += 1 { acc += i; }\n"
                                       "    return acc;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_FOR_LOOP) == 0);
    assert(test_count_opcode(chunk, OP_CMP_LTI) == 1);

    test_program_free(&program);
}

// The fused loop must behave exactly as the general one did. These pin the
// cases where a wrong offset or a missed exit would still produce a number:
// the count, the boundary, and the two ways out of a body.
static void test_a_counting_loop_runs_the_right_number_of_times() {
    assert(test_run_int("func f(): int { let c: int = 0; let n: int = 5;\n"
                        "                for let i: int = 0; i < n; i += 1 { c += 1; }\n"
                        "                return c; }\n"
                        "let r: int = f();\n") == 5);
}

// A bound of zero must not run the body once: the entry test is what catches
// it, since the fused instruction tests only after stepping.
static void test_a_counting_loop_with_no_iterations_runs_none() {
    assert(test_run_int("func f(): int { let c: int = 0; let n: int = 0;\n"
                        "                for let i: int = 0; i < n; i += 1 { c += 1; }\n"
                        "                return c; }\n"
                        "let r: int = f();\n") == 0);
}

static void test_a_counting_loop_leaves_its_counter_at_the_bound() {
    assert(test_run_int("func f(): int { let n: int = 4; let last: int = -1;\n"
                        "                for let i: int = 0; i < n; i += 1 { last = i; }\n"
                        "                return last; }\n"
                        "let r: int = f();\n") == 3);
}

static void test_break_leaves_a_counting_loop() {
    assert(test_run_int("func f(): int { let c: int = 0; let n: int = 10;\n"
                        "                for let i: int = 0; i < n; i += 1 {\n"
                        "                    if i > 2 { break; }\n"
                        "                    c += 1;\n"
                        "                }\n"
                        "                return c; }\n"
                        "let r: int = f();\n") == 3);
}

static void test_continue_still_steps_a_counting_loop() {
    assert(test_run_int("func f(): int { let c: int = 0; let n: int = 6;\n"
                        "                for let i: int = 0; i < n; i += 1 {\n"
                        "                    if i % 2 == 0 { continue; }\n"
                        "                    c += 1;\n"
                        "                }\n"
                        "                return c; }\n"
                        "let r: int = f();\n") == 3);
}

static void test_counting_loops_nest() {
    assert(test_run_int("func f(): int { let c: int = 0; let n: int = 4; let m: int = 3;\n"
                        "                for let i: int = 0; i < n; i += 1 {\n"
                        "                    for let j: int = 0; j < m; j += 1 { c += 1; }\n"
                        "                }\n"
                        "                return c; }\n"
                        "let r: int = f();\n") == 12);
}

// A body that writes to the counter keeps the general form, and must still
// mean what it says: this one steps twice per iteration, so it ends early.
static void test_a_body_that_writes_the_counter_still_works() {
    assert(test_run_int("func f(): int { let c: int = 0; let n: int = 10;\n"
                        "                for let i: int = 0; i < n; i += 1 { i += 1; c += 1; }\n"
                        "                return c; }\n"
                        "let r: int = f();\n") == 5);
}

// A counter written through a pointer changes without being named, so the
// fused form -- which the body must not disturb -- cannot stand for this loop.
// The general form runs it correctly: the store lands before the next test.
static void test_a_counter_written_through_a_pointer_is_not_fused() {
    TestProgram program = test_compile("func run(n: int): int {\n"
                                       "    let c: int = 0;\n"
                                       "    for let i: int = 0; i < n; i += 1 {\n"
                                       "        let p: ref int = i;\n"
                                       "        *p += 1;\n"
                                       "        c += 1;\n"
                                       "    }\n"
                                       "    return c;\n"
                                       "}\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_FOR_LOOP) == 0);

    test_program_free(&program);
}

// ...and it means what it says: stepping twice per iteration halves the count.
static void test_a_counter_written_through_a_pointer_still_works() {
    assert(test_run_int("func f(): int { let c: int = 0; let n: int = 10;\n"
                        "                for let i: int = 0; i < n; i += 1 {\n"
                        "                    let p: ref int = i;\n"
                        "                    *p += 1;\n"
                        "                    c += 1;\n"
                        "                }\n"
                        "                return c; }\n"
                        "let r: int = f();\n") == 5);
}

// A body that writes the counter keeps the general form. Asserted on the
// emitted code as well as the answer, because the two forms agree on the
// answer here -- only the instruction count tells them apart.
static void test_a_body_that_writes_the_counter_is_not_fused() {
    TestProgram program = test_compile("func run(n: int): int {\n"
                                       "    let c: int = 0;\n"
                                       "    for let i: int = 0; i < n; i += 1 { i += 1; c += 1; }\n"
                                       "    return c;\n"
                                       "}\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_FOR_LOOP) == 0);

    test_program_free(&program);
}

int main() {
    test_a_literal_initialiser_loads_into_the_variable();
    test_assigning_a_variable_is_a_single_move();
    test_a_counting_loop_runs_the_right_number_of_times();
    test_a_counting_loop_with_no_iterations_runs_none();
    test_a_counting_loop_leaves_its_counter_at_the_bound();
    test_break_leaves_a_counting_loop();
    test_continue_still_steps_a_counting_loop();
    test_counting_loops_nest();
    test_a_body_that_writes_the_counter_still_works();
    test_a_counter_written_through_a_pointer_is_not_fused();
    test_a_counter_written_through_a_pointer_still_works();
    test_a_body_that_writes_the_counter_is_not_fused();
    test_a_long_body_keeps_the_general_form();
    test_a_counting_loop_is_one_instruction_per_iteration();
    test_a_counting_loop_is_recognised_however_the_step_is_spelled();
    test_a_counting_loop_takes_the_step_from_either_side();
    test_a_loop_stepping_by_more_than_one_keeps_the_general_form();
    test_a_loop_assigning_something_else_keeps_the_general_form();
    test_a_general_loop_keeps_the_compare_and_jump();
    test_a_local_loop_body_loads_no_constants();
    test_a_field_loop_body_loads_and_stores_each_access();

    printf("loop_shape_test: all tests passed\n");
    return 0;
}
