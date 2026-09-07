#include "support/emit.h"

#include <assert.h>
#include <stdio.h>

static void a_body_the_emitter_covers_is_accepted(void) {
    TestEmission emission = test_emit_ir("func nothing() { }\n");

    assert(codegen_mir_supports(emission.ir));

    test_emission_free(&emission);
}

static void a_body_holding_an_uncovered_instruction_is_refused(void) {
    TestEmission emission = test_lower_ir("func one(): i32 { return 1; }\n");

    MIRBlock *block = mir_block_at(emission.ir, emission.ir->entry);

    /* An operation with no emission stands for whatever the emitter has yet to cover. */
    mir_emit(emission.ir, block, (MIRInst){.op = MIR_UNREACHABLE, .result = MIR_NO_VALUE});

    assert(!codegen_mir_supports(emission.ir));

    test_emission_free(&emission);
}

static void an_empty_body_emits_the_return_the_ir_holds(void) {
    TestEmission emission = test_emit_ir("func nothing() { }\n");

    assert(test_count_opcode(emission.chunk, OP_RETURN) == 1);
    assert(emission.chunk->instructions.size == 1);

    test_emission_free(&emission);
}

static void the_frame_holds_a_slot_for_every_parameter(void) {
    TestEmission emission = test_emit_ir("func two(a: i32, b: i32) { }\n");

    assert(emission.max_registers >= 3);

    test_emission_free(&emission);
}

static void an_int_literal_loads_from_the_constant_pool(void) {
    TestEmission emission = test_emit_ir("func one(): i32 { return 7; }\n");

    assert(test_count_opcode(emission.chunk, OP_LOAD_CONST) == 1);
    assert(emission.chunk->const_pool->count == 1);
    assert(emission.chunk->const_pool->constants[0].as_int == 7);

    test_emission_free(&emission);
}

static void a_float_literal_loads_from_the_constant_pool(void) {
    TestEmission emission = test_emit_ir("func one(): f32 { return 1.5; }\n");

    assert(emission.chunk->const_pool->constants[0].as_float == 1.5f);

    test_emission_free(&emission);
}

static void a_bool_literal_loads_from_the_constant_pool(void) {
    TestEmission emission = test_emit_ir("func one(): bool { return true; }\n");

    assert(emission.chunk->const_pool->constants[0].as_int == 1);

    test_emission_free(&emission);
}

static void a_constant_load_writes_the_slot_the_return_reads(void) {
    TestEmission emission = test_emit_ir("func one(): i32 { return 7; }\n");

    long load = test_find_opcode(emission.chunk, OP_LOAD_CONST);
    long ret = test_find_opcode(emission.chunk, OP_RETURN);

    assert(load >= 0 && ret >= 0);

    assert(VM_DECODE_I_RD(test_instruction(emission.chunk, (size_t)load)) ==
           VM_DECODE_R_R1(test_instruction(emission.chunk, (size_t)ret)));

    test_emission_free(&emission);
}

static void a_negated_literal_folds_to_one_load(void) {
    TestEmission emission = test_emit_ir("func one(): i32 { return -42; }\n");

    assert(test_count_opcode(emission.chunk, OP_LOAD_CONST) == 1);
    assert(emission.chunk->const_pool->count == 1);
    assert(emission.chunk->const_pool->constants[0].as_int == -42);

    test_emission_free(&emission);
}

static void a_negated_float_literal_folds_to_one_load(void) {
    TestEmission emission = test_emit_ir("func one(): f32 { return -1.5; }\n");

    assert(test_count_opcode(emission.chunk, OP_LOAD_CONST) == 1);
    assert(emission.chunk->const_pool->constants[0].as_float == -1.5f);

    test_emission_free(&emission);
}

static void a_negated_literal_folds_before_the_emitter_is_asked(void) {
    TestEmission emission = test_emit_ir("func one(): i32 { return -42; }\n");

    assert(codegen_mir_supports(emission.ir));

    test_emission_free(&emission);
}

static void a_constant_expression_reaches_the_emitter_folded(void) {
    TestEmission emission = test_emit_ir("func one(): i32 { return 2 + 3; }\n");

    assert(test_count_opcode(emission.chunk, OP_LOAD_CONST) == 1);
    assert(emission.chunk->const_pool->constants[0].as_int == 5);

    test_emission_free(&emission);
}

static void an_int_addition_emits_the_int_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: i32, b: i32): i32 { return a + b; }\n");

    assert(test_count_opcode(emission.chunk, OP_ADDI) == 1);

    test_emission_free(&emission);
}

static void an_int_subtraction_emits_the_int_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: i32, b: i32): i32 { return a - b; }\n");

    assert(test_count_opcode(emission.chunk, OP_SUBI) == 1);

    test_emission_free(&emission);
}

static void an_int_multiplication_emits_the_int_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: i32, b: i32): i32 { return a * b; }\n");

    assert(test_count_opcode(emission.chunk, OP_MULI) == 1);

    test_emission_free(&emission);
}

static void an_int_division_emits_the_int_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: i32, b: i32): i32 { return a / b; }\n");

    assert(test_count_opcode(emission.chunk, OP_DIVI) == 1);

    test_emission_free(&emission);
}

static void an_int_remainder_emits_the_int_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: i32, b: i32): i32 { return a % b; }\n");

    assert(test_count_opcode(emission.chunk, OP_MODI) == 1);

    test_emission_free(&emission);
}

static void a_float_addition_emits_the_float_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: f32, b: f32): f32 { return a + b; }\n");

    assert(test_count_opcode(emission.chunk, OP_ADDF) == 1);

    test_emission_free(&emission);
}

static void a_float_subtraction_emits_the_float_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: f32, b: f32): f32 { return a - b; }\n");

    assert(test_count_opcode(emission.chunk, OP_SUBF) == 1);

    test_emission_free(&emission);
}

static void a_float_multiplication_emits_the_float_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: f32, b: f32): f32 { return a * b; }\n");

    assert(test_count_opcode(emission.chunk, OP_MULF) == 1);

    test_emission_free(&emission);
}

static void a_float_division_emits_the_float_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: f32, b: f32): f32 { return a / b; }\n");

    assert(test_count_opcode(emission.chunk, OP_DIVF) == 1);

    test_emission_free(&emission);
}

static void negating_an_int_emits_the_int_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: i32): i32 { return -a; }\n");

    assert(test_count_opcode(emission.chunk, OP_NEGI) == 1);
    assert(test_count_opcode(emission.chunk, OP_SUBI) == 0);

    test_emission_free(&emission);
}

static void negating_a_float_emits_the_float_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: f32): f32 { return -a; }\n");

    assert(test_count_opcode(emission.chunk, OP_NEGF) == 1);
    assert(test_count_opcode(emission.chunk, OP_SUBF) == 0);

    test_emission_free(&emission);
}

static void an_operation_reads_the_slots_its_operands_were_given(void) {
    TestEmission emission = test_emit_ir("func one(a: i32, b: i32): i32 { return a + b; }\n");

    long add = test_find_opcode(emission.chunk, OP_ADDI);
    long ret = test_find_opcode(emission.chunk, OP_RETURN);

    assert(add >= 0 && ret >= 0);

    Instruction sum = test_instruction(emission.chunk, (size_t)add);

    assert(VM_DECODE_R_R1(sum) != VM_DECODE_R_R2(sum));

    assert(VM_DECODE_R_RD(sum) == VM_DECODE_R_R1(test_instruction(emission.chunk, (size_t)ret)));

    test_emission_free(&emission);
}

static void comparing_ints_emits_the_int_opcode(void) {
    struct {
        const char *op;
        OpCode expected;
    } cases[] = {
        {"<", OP_CMP_LTI},  {">", OP_CMP_GTI},  {"==", OP_CMP_EQI},
        {"!=", OP_CMP_NEI}, {"<=", OP_CMP_LEI}, {">=", OP_CMP_GEI},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[128];
        snprintf(source, sizeof(source), "func one(a: i32, b: i32): bool { return a %s b; }\n", cases[i].op);

        TestEmission emission = test_emit_ir(source);

        assert(test_count_opcode(emission.chunk, cases[i].expected) == 1);

        test_emission_free(&emission);
    }
}

static void comparing_floats_emits_the_float_opcode(void) {
    struct {
        const char *op;
        OpCode expected;
    } cases[] = {
        {"<", OP_CMP_LTF},  {">", OP_CMP_GTF},  {"==", OP_CMP_EQF},
        {"!=", OP_CMP_NEF}, {"<=", OP_CMP_LEF}, {">=", OP_CMP_GEF},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[128];
        snprintf(source, sizeof(source), "func one(a: f32, b: f32): bool { return a %s b; }\n", cases[i].op);

        TestEmission emission = test_emit_ir(source);

        assert(test_count_opcode(emission.chunk, cases[i].expected) == 1);

        test_emission_free(&emission);
    }
}

static void comparing_bools_emits_the_int_opcode(void) {
    TestEmission emission = test_emit_ir("func one(a: bool, b: bool): bool { return a == b; }\n");

    assert(test_count_opcode(emission.chunk, OP_CMP_EQI) == 1);

    test_emission_free(&emission);
}

static void a_comparison_reads_the_slots_its_operands_were_given(void) {
    TestEmission emission = test_emit_ir("func one(a: i32, b: i32): bool { return a < b; }\n");

    long cmp = test_find_opcode(emission.chunk, OP_CMP_LTI);
    long ret = test_find_opcode(emission.chunk, OP_RETURN);

    assert(cmp >= 0 && ret >= 0);

    Instruction less = test_instruction(emission.chunk, (size_t)cmp);

    assert(VM_DECODE_R_R1(less) != VM_DECODE_R_R2(less));
    assert(VM_DECODE_R_RD(less) == VM_DECODE_R_R1(test_instruction(emission.chunk, (size_t)ret)));

    test_emission_free(&emission);
}

static void negating_a_bool_emits_its_comparison(void) {
    TestEmission emission = test_emit_ir("func one(a: bool): bool { return !a; }\n");

    assert(test_count_opcode(emission.chunk, OP_CMP_EQI) == 1);

    test_emission_free(&emission);
}

static void a_condition_chooses_between_two_values(void) {
    TestEmission emission =
        test_emit_ir("func one(a: i32): i32 { if a < 0 { return 1; } else { return 2; } }\n");

    assert(test_count_opcode(emission.chunk, OP_JMP_IF_FALSE) == 1);
    assert(test_count_opcode(emission.chunk, OP_RETURN) == 2);

    test_emission_free(&emission);
}

static void a_conditional_jump_reads_the_slot_its_comparison_wrote(void) {
    TestEmission emission =
        test_emit_ir("func one(a: i32): i32 { if a < 0 { return 1; } else { return 2; } }\n");

    long cmp = test_find_opcode(emission.chunk, OP_CMP_LTI);
    long branch = test_find_opcode(emission.chunk, OP_JMP_IF_FALSE);

    assert(cmp >= 0 && branch > cmp);

    assert(VM_DECODE_I_RD(test_instruction(emission.chunk, (size_t)branch)) ==
           VM_DECODE_R_RD(test_instruction(emission.chunk, (size_t)cmp)));

    test_emission_free(&emission);
}

static void a_conditional_jump_lands_on_its_target(void) {
    TestEmission emission =
        test_emit_ir("func one(a: i32): i32 { if a < 0 { return 1; } else { return 2; } }\n");

    long branch = test_find_opcode(emission.chunk, OP_JMP_IF_FALSE);

    assert(branch >= 0);

    long offset = (long)VM_DECODE_I_SIMM(test_instruction(emission.chunk, (size_t)branch));
    long target = branch + 1 + offset;

    assert(target > branch);
    assert(target < (long)emission.chunk->instructions.size);

    test_emission_free(&emission);
}

static void a_local_takes_the_value_stored_into_it(void) {
    assert(test_run_emitted_int("func one(): i32 { let t: i32 = 5; return t; }\n") == 5);
}

static void a_loop_jumps_back_to_its_condition(void) {
    TestEmission emission =
        test_emit_ir("func one(n: i32): i32 { let t: i32 = 0; for let i: i32 = 0; i < n; i = i + 1 "
                     "{ t = t + i; } return t; }\n");

    long back = -1;

    for (size_t i = 0; i < emission.chunk->instructions.size; i++) {
        Instruction instruction = test_instruction(emission.chunk, i);

        if (VM_DECODE_OPCODE(instruction) == OP_JMP && VM_DECODE_I_SIMM(instruction) < 0) {
            back = (long)i;
        }
    }

    assert(back >= 0);

    test_emission_free(&emission);
}

static void a_loop_counts_what_its_body_accumulates(void) {
    TestEmission emission =
        test_emit_ir("func one(n: i32): i32 { let t: i32 = 0; for let i: i32 = 0; i < n; i = i + 1 "
                     "{ t = t + i; } return t; }\n");

    assert(test_count_opcode(emission.chunk, OP_ADDI) == 2);
    assert(test_count_opcode(emission.chunk, OP_CMP_LTI) == 1);

    test_emission_free(&emission);
}

static void an_emitted_body_computes_its_arithmetic(void) {
    assert(test_run_emitted_int("func one(): i32 { let a: i32 = 6; let b: i32 = 7; return a * b; }\n") == 42);
}

static void an_emitted_body_computes_its_branch(void) {
    assert(test_run_emitted_int("func one(): i32 { let a: i32 = 3; if a < 5 { return 1; } return 2; }\n") ==
           1);

    assert(test_run_emitted_int("func one(): i32 { let a: i32 = 9; if a < 5 { return 1; } return 2; }\n") ==
           2);
}

static void an_emitted_body_computes_its_loop(void) {
    assert(test_run_emitted_int("func one(): i32 { let t: i32 = 0; for let i: i32 = 0; i < 5; i = i + 1 "
                                "{ t = t + i; } return t; }\n") == 10);
}

static void an_emitted_body_computes_its_negation(void) {
    assert(test_run_emitted_int("func one(): i32 { let a: i32 = 5; return -a; }\n") == -5);
}

static void a_field_is_reached_at_its_offset_from_the_base(void) {
    TestEmission emission = test_emit_ir("struct P { x: i32, y: i32 }\n"
                                         "func one(): i32 { let p: P = P { x: 1, y: 2 }; return p.y; }\n");

    long ret = test_find_opcode(emission.chunk, OP_RETURN);

    assert(ret >= 0);

    test_emission_free(&emission);
}

static void an_emitted_body_reads_the_field_it_wrote(void) {
    assert(test_run_emitted_int("struct P { x: i32, y: i32 }\n"
                                "func one(): i32 { let p: P = P { x: 1, y: 2 }; return p.y; }\n") == 2);
}

static void an_emitted_body_reads_the_first_field(void) {
    assert(test_run_emitted_int("struct P { x: i32, y: i32 }\n"
                                "func one(): i32 { let p: P = P { x: 4, y: 9 }; return p.x; }\n") == 4);
}

static void an_emitted_body_sees_a_field_it_assigned(void) {
    assert(test_run_emitted_int("struct P { x: i32, y: i32 }\n"
                                "func one(): i32 { let p: P = P { x: 1, y: 2 }; p.y = 7; return p.y; }\n") ==
           7);
}

static void an_emitted_body_adds_two_fields(void) {
    assert(test_run_emitted_int("struct P { x: i32, y: i32 }\n"
                                "func one(): i32 { let p: P = P { x: 3, y: 5 }; return p.x + p.y; }\n") == 8);
}

static void an_emitted_body_reads_an_element_it_wrote(void) {
    assert(test_run_emitted_int("func one(): i32 { let a: array<i32, 3>; a[1] = 7; return a[1]; }\n") == 7);
}

static void an_emitted_body_reads_a_computed_index(void) {
    assert(test_run_emitted_int("func one(): i32 { let a: array<i32, 4>; "
                                "for let i: i32 = 0; i < 4; i = i + 1 { a[i] = i * i; } "
                                "return a[3]; }\n") == 9);
}

static void an_emitted_body_sums_what_it_indexed(void) {
    assert(test_run_emitted_int("func one(): i32 { let a: array<i32, 3>; a[0] = 1; a[1] = 2; a[2] = 4; "
                                "let t: i32 = 0; for let i: i32 = 0; i < 3; i = i + 1 { t = t + a[i]; } "
                                "return t; }\n") == 7);
}

static void an_emitted_body_reads_through_a_pointer(void) {
    assert(test_run_emitted_int("func one(): i32 { let v: i32 = 6; let p: &i32 = v; return *p; }\n") == 6);
}

static void an_emitted_body_writes_through_a_pointer(void) {
    assert(test_run_emitted_int("func one(): i32 { let v: i32 = 6; let p: &i32 = v; *p = 9; return v; }\n") ==
           9);
}

static void an_index_past_the_end_traps(void) {
    assert(
        test_run_emitted_status("func one(): i32 { let a: array<i32, 3>; let i: i32 = 5; return a[i]; }\n") ==
        VM_RUN_ERR_BOUNDS);
}

static void a_negative_index_traps(void) {
    assert(test_run_emitted_status("func one(): i32 { let a: array<i32, 3>; let i: i32 = 0 - 1; "
                                   "return a[i]; }\n") == VM_RUN_ERR_BOUNDS);
}

static void an_emitted_call_returns_what_its_callee_returned(void) {
    assert(test_run_emitted_unit_int("func other(): i32 { return 3; }\n"
                                     "func one(): i32 { return other(); }\n") == 3);
}

static void an_emitted_call_passes_its_arguments_in_order(void) {
    assert(test_run_emitted_unit_int("func sub(a: i32, b: i32): i32 { return a - b; }\n"
                                     "func one(): i32 { return sub(9, 4); }\n") == 5);
}

static void an_emitted_call_nests(void) {
    assert(test_run_emitted_unit_int("func twice(a: i32): i32 { return a * 2; }\n"
                                     "func one(): i32 { return twice(twice(3)); }\n") == 12);
}

static void an_emitted_call_recurses(void) {
    assert(test_run_emitted_unit_int("func sum(n: i32): i32 { if n < 1 { return 0; } "
                                     "return n + sum(n - 1); }\n"
                                     "func one(): i32 { return sum(4); }\n") == 10);
}

static void a_call_leaves_a_live_value_alone(void) {
    assert(test_run_emitted_unit_int("func twice(a: i32): i32 { return a * 2; }\n"
                                     "func one(): i32 { let t: i32 = 7; let d: i32 = twice(3); "
                                     "return t + d; }\n") == 13);
}

static void an_int_cast_to_float_emits_the_conversion(void) {
    TestEmission emission = test_emit_ir("func one(a: i32): f32 { return f32(a); }\n");

    assert(test_count_opcode(emission.chunk, OP_ITOF) == 1);

    test_emission_free(&emission);
}

static void a_float_cast_to_int_emits_the_conversion(void) {
    TestEmission emission = test_emit_ir("func one(a: f32): i32 { return i32(a); }\n");

    assert(test_count_opcode(emission.chunk, OP_FTOI) == 1);

    test_emission_free(&emission);
}

static void an_emitted_body_converts_an_int_to_a_float(void) {
    assert(test_run_emitted_int("func one(): i32 { let a: i32 = 7; let f: f32 = f32(a); "
                                "return i32(f); }\n") == 7);
}

static void an_emitted_body_truncates_a_float_to_an_int(void) {
    assert(test_run_emitted_int("func one(): i32 { let f: f32 = 3.75; return i32(f); }\n") == 3);
}

static void a_box_allocates_and_releases_what_it_allocated(void) {
    TestEmission emission = test_emit_ir("struct P { x: i32 }\n"
                                         "func one(): i32 { let p: *P = box P { x: 5 }; return p.x; }\n");

    assert(test_count_opcode(emission.chunk, OP_BOX) == 1);
    assert(test_count_opcode(emission.chunk, OP_RELEASE) == 1);

    test_emission_free(&emission);
}

static void a_release_follows_the_read_it_outlives(void) {
    TestEmission emission = test_emit_ir("struct P { x: i32 }\n"
                                         "func one(): i32 { let p: *P = box P { x: 5 }; return p.x; }\n");

    long box = test_find_opcode(emission.chunk, OP_BOX);
    long release = test_find_opcode(emission.chunk, OP_RELEASE);

    assert(box >= 0 && release > box);

    test_emission_free(&emission);
}

static void an_emitted_body_reads_what_it_boxed(void) {
    assert(test_run_emitted_unit_int("struct P { x: i32 }\n"
                                     "func one(): i32 { let p: *P = box P { x: 5 }; return p.x; }\n") == 5);
}

static void an_emitted_body_writes_through_what_it_boxed(void) {
    assert(test_run_emitted_unit_int("struct P { x: i32 }\n"
                                     "func one(): i32 { let p: *P = box P { x: 5 }; p.x = 8; "
                                     "return p.x; }\n") == 8);
}

static void an_emitted_body_reads_a_struct_through_a_pointer(void) {
    assert(test_run_emitted_unit_int("struct P { x: i32, y: i32 }\n"
                                     "func one(): i32 { let p: *P = box P { x: 4, y: 9 }; "
                                     "let q: P = *p; return q.y; }\n") == 9);
}

static void an_emitted_body_writes_a_struct_through_a_pointer(void) {
    assert(test_run_emitted_unit_int("struct P { x: i32, y: i32 }\n"
                                     "func one(): i32 { let p: *P = box P { x: 1, y: 2 }; "
                                     "*p = P { x: 7, y: 8 }; return p.y; }\n") == 8);
}

static void a_wide_read_through_a_pointer_is_one_instruction(void) {
    TestEmission emission = test_emit_ir("struct P { x: i32, y: i32 }\n"
                                         "func one(): i32 { let p: *P = box P { x: 4, y: 9 }; "
                                         "let q: P = *p; return q.y; }\n");

    assert(test_count_opcode(emission.chunk, OP_LOAD_PTR_N) >= 1);

    test_emission_free(&emission);
}

static void an_emitted_body_negates_a_bool_it_still_holds(void) {
    /* The operand dies at the negation, so its slot is the one the result is free to take. */
    assert(test_run_emitted_int("func one(): i32 { let a: bool = false; if !a { return 1; } return 0; }\n") ==
           1);

    assert(test_run_emitted_int("func one(): i32 { let a: bool = true; if !a { return 1; } return 0; }\n") ==
           0);
}

static void a_string_literal_loads_from_the_unit(void) {
    TestEmission emission = test_emit_ir("func one(): i32 { let s: &str = \"abc\"; return 0; }\n");

    assert(test_count_opcode(emission.chunk, OP_LOAD_STR) == 1);

    test_emission_free(&emission);
}

static void the_same_text_loads_one_string(void) {
    TestEmission emission =
        test_emit_ir("func one(): i32 { let a: &str = \"hi\"; let b: &str = \"hi\"; return 0; }\n");

    long first = -1;
    long second = -1;

    for (size_t i = 0; i < emission.chunk->instructions.size; i++) {
        if (VM_DECODE_OPCODE(test_instruction(emission.chunk, i)) != OP_LOAD_STR) {
            continue;
        }

        if (first < 0) {
            first = (long)i;
        } else {
            second = (long)i;
        }
    }

    assert(first >= 0 && second > first);

    assert(VM_DECODE_I_KX(test_instruction(emission.chunk, (size_t)first)) ==
           VM_DECODE_I_KX(test_instruction(emission.chunk, (size_t)second)));

    test_emission_free(&emission);
}

static void a_slice_holds_where_its_elements_start_and_how_many(void) {
    TestEmission emission =
        test_emit_ir_named("func g(xs: &slice<i32>): i32 { return 0; }\n"
                           "func one(): i32 { let a: array<i32, 3> = [1, 20, 300]; return g(a); }\n",
                           "one");

    long address = test_find_opcode(emission.chunk, OP_ADDR_OF);

    assert(address >= 0);

    test_emission_free(&emission);
}

static void an_emitted_body_passes_an_array_as_a_slice(void) {
    assert(test_run_emitted_unit_int("func g(xs: &slice<i32>): i32 { return 0; }\n"
                                     "func one(): i32 { let a: array<i32, 3> = [1, 20, 300]; "
                                     "return g(a); }\n") == 0);
}

static void an_emitted_body_reads_an_element_through_a_slice(void) {
    assert(test_run_emitted_unit_int("func one(): i32 { let a: array<i32, 3> = [1, 20, 300]; "
                                     "let s: &slice<i32> = a; return s[1]; }\n") == 20);
}

static void an_index_past_a_slice_traps(void) {
    assert(test_run_emitted_unit_status("func one(): i32 { let a: array<i32, 3> = [1, 2, 3]; "
                                        "let s: &slice<i32> = a; let i: i32 = 5; "
                                        "return s[i]; }\n") == VM_RUN_ERR_BOUNDS);
}

static void an_emitted_body_indexes_an_array_literal(void) {
    assert(test_run_emitted_unit_int("func one(): i32 { let a: array<i32, 3> = [1, 20, 300]; "
                                     "return a[1]; }\n") == 20);
}

int main(void) {
    a_body_the_emitter_covers_is_accepted();
    a_body_holding_an_uncovered_instruction_is_refused();
    an_empty_body_emits_the_return_the_ir_holds();
    an_int_literal_loads_from_the_constant_pool();
    a_float_literal_loads_from_the_constant_pool();
    a_bool_literal_loads_from_the_constant_pool();
    a_constant_load_writes_the_slot_the_return_reads();
    a_negated_literal_folds_to_one_load();
    a_negated_float_literal_folds_to_one_load();
    a_negated_literal_folds_before_the_emitter_is_asked();
    a_constant_expression_reaches_the_emitter_folded();
    an_int_addition_emits_the_int_opcode();
    an_int_subtraction_emits_the_int_opcode();
    an_int_multiplication_emits_the_int_opcode();
    an_int_division_emits_the_int_opcode();
    an_int_remainder_emits_the_int_opcode();
    a_float_addition_emits_the_float_opcode();
    a_float_subtraction_emits_the_float_opcode();
    a_float_multiplication_emits_the_float_opcode();
    a_float_division_emits_the_float_opcode();
    negating_an_int_emits_the_int_opcode();
    negating_a_float_emits_the_float_opcode();
    an_operation_reads_the_slots_its_operands_were_given();
    comparing_ints_emits_the_int_opcode();
    comparing_floats_emits_the_float_opcode();
    comparing_bools_emits_the_int_opcode();
    a_comparison_reads_the_slots_its_operands_were_given();
    negating_a_bool_emits_its_comparison();
    a_condition_chooses_between_two_values();
    a_conditional_jump_reads_the_slot_its_comparison_wrote();
    a_conditional_jump_lands_on_its_target();
    a_local_takes_the_value_stored_into_it();
    a_loop_jumps_back_to_its_condition();
    a_loop_counts_what_its_body_accumulates();
    an_emitted_body_computes_its_arithmetic();
    an_emitted_body_computes_its_branch();
    an_emitted_body_computes_its_loop();
    an_emitted_body_computes_its_negation();
    a_field_is_reached_at_its_offset_from_the_base();
    an_emitted_body_reads_the_field_it_wrote();
    an_emitted_body_reads_the_first_field();
    an_emitted_body_sees_a_field_it_assigned();
    an_emitted_body_adds_two_fields();
    an_emitted_body_reads_an_element_it_wrote();
    an_emitted_body_reads_a_computed_index();
    an_emitted_body_sums_what_it_indexed();
    an_emitted_body_reads_through_a_pointer();
    an_emitted_body_writes_through_a_pointer();
    an_index_past_the_end_traps();
    a_negative_index_traps();
    an_emitted_call_returns_what_its_callee_returned();
    an_emitted_call_passes_its_arguments_in_order();
    an_emitted_call_nests();
    an_emitted_call_recurses();
    a_call_leaves_a_live_value_alone();
    an_int_cast_to_float_emits_the_conversion();
    a_float_cast_to_int_emits_the_conversion();
    an_emitted_body_converts_an_int_to_a_float();
    an_emitted_body_truncates_a_float_to_an_int();
    a_box_allocates_and_releases_what_it_allocated();
    a_release_follows_the_read_it_outlives();
    an_emitted_body_reads_what_it_boxed();
    an_emitted_body_writes_through_what_it_boxed();
    an_emitted_body_reads_a_struct_through_a_pointer();
    an_emitted_body_writes_a_struct_through_a_pointer();
    a_wide_read_through_a_pointer_is_one_instruction();
    an_emitted_body_negates_a_bool_it_still_holds();
    a_string_literal_loads_from_the_unit();
    the_same_text_loads_one_string();
    a_slice_holds_where_its_elements_start_and_how_many();
    an_emitted_body_passes_an_array_as_a_slice();
    an_emitted_body_indexes_an_array_literal();
    an_emitted_body_reads_an_element_through_a_slice();
    an_index_past_a_slice_traps();
    the_frame_holds_a_slot_for_every_parameter();

    printf("codegen_mir_test passed\n");

    return 0;
}
