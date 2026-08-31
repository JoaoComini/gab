#include "support/run.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_a_constant_expression_folds_to_one_load() {
    TestProgram program = test_compile("let x: int = 2 + 3;\n");
    Chunk *chunk = test_top_chunk(&program);

    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);
    assert(test_count_opcode(chunk, OP_ADDI) == 0);

    assert(chunk->const_pool->count == 1);
    assert(chunk->const_pool->constants[0].as_int == 5);

    test_program_free(&program);
}

static void test_a_constant_float_expression_folds() {
    TestProgram program = test_compile("let x: float = 0.0 - 9.8;\n");
    Chunk *chunk = test_top_chunk(&program);

    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);
    assert(test_count_opcode(chunk, OP_SUBF) == 0);
    assert(test_count_opcode(chunk, OP_SUBFK) == 0);

    assert(chunk->const_pool->count == 1);
    assert(chunk->const_pool->constants[0].as_float == -9.8f);

    test_program_free(&program);
}

static void test_a_nested_constant_expression_folds_wholly() {
    TestProgram program = test_compile("let x: int = 2 + 3 * 4;\n"
                                       "let y: float = 1.0 + 2.0 + 3.0;\n");

    Chunk *chunk = test_top_chunk(&program);

    assert(test_count_opcode(chunk, OP_ADDI) == 0);
    assert(test_count_opcode(chunk, OP_MULI) == 0);
    assert(test_count_opcode(chunk, OP_ADDF) == 0);
    assert(test_count_opcode(chunk, OP_ADDFK) == 0);

    test_program_free(&program);
}

static void test_a_variable_operand_does_not_fold() {
    TestProgram program = test_compile("let a: int = 2;\n"
                                       "let x: int = a + 3;\n");

    assert(test_count_opcode(test_top_chunk(&program), OP_ADDI) == 1);

    test_program_free(&program);
}

static void test_a_constant_division_by_zero_does_not_fold() {
    TestProgram program = test_compile("func f(): int { return 1 / 0; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_DIVI) == 1);

    test_program_free(&program);
}

static void test_negated_literal_folds_to_one_load() {
    TestProgram program = test_compile("let x: int = -42;\n");
    Chunk *chunk = test_top_chunk(&program);

    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);
    assert(test_count_opcode(chunk, OP_SUBI) == 0);

    assert(chunk->const_pool->count == 1);
    assert(chunk->const_pool->constants[0].as_int == -42);

    test_program_free(&program);
}

static void test_negating_a_variable_emits_one_instruction() {
    TestProgram program = test_compile("let a: int = 42;\n"
                                       "let x: int = -a;\n");

    Chunk *chunk = test_top_chunk(&program);

    assert(test_count_opcode(chunk, OP_NEGI) == 1);
    assert(test_count_opcode(chunk, OP_SUBI) == 0);

    assert(chunk->const_pool->count == 1);

    test_program_free(&program);
}

static void test_negating_a_float_variable_emits_one_instruction() {
    TestProgram program = test_compile("let a: float = 1.5;\n"
                                       "let x: float = -a;\n");

    Chunk *chunk = test_top_chunk(&program);

    assert(test_count_opcode(chunk, OP_NEGF) == 1);
    assert(test_count_opcode(chunk, OP_SUBF) == 0);

    test_program_free(&program);
}

static void test_a_float_compared_to_a_literal_uses_the_constant_form() {
    TestProgram program = test_compile("func f(a: float): bool { return a < 0.0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_CMP_LTFK) == 1);
    assert(test_count_opcode(chunk, OP_CMP_LTF) == 0);

    long at = test_find_opcode(chunk, OP_CMP_LTFK);
    assert(VM_DECODE_R_K(test_instruction(chunk, (size_t)at)) == 0);

    test_program_free(&program);
}

static void test_a_binary_op_reads_its_operands() {
    TestProgram program = test_compile("let a: float = 10.0;\n"
                                       "let b: float = 5.0;\n"
                                       "let c: bool = a > b;\n");

    Chunk *chunk = test_top_chunk(&program);

    long cmp_index = test_find_opcode(chunk, OP_CMP_GTF);
    assert(cmp_index >= 0);

    Instruction cmp = test_instruction(chunk, (size_t)cmp_index);

    unsigned int r1 = VM_DECODE_R_R1(cmp);
    unsigned int r2 = VM_DECODE_R_R2(cmp);

    assert(r1 != r2);

    test_program_free(&program);
}

static void test_a_small_literal_becomes_an_immediate() {
    TestProgram program = test_compile("let a: int = 10;\n"
                                       "let b: int = a + 1;\n");

    Chunk *chunk = test_top_chunk(&program);

    long add_index = test_find_opcode(chunk, OP_ADDI);
    assert(add_index >= 0);

    Instruction add = test_instruction(chunk, (size_t)add_index);
    assert(VM_DECODE_R_K(add) == 1);
    assert(VM_DECODE_R_R2(add) == 1);

    assert(chunk->const_pool->count == 1);

    test_program_free(&program);
}

static void test_a_compound_assignment_takes_an_immediate() {
    TestProgram program = test_compile("let a: int = 10;\n"
                                       "func f() { let b: int = 1; b += 1; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    long add_index = test_find_opcode(chunk, OP_ADDI);
    assert(add_index >= 0);

    Instruction add = test_instruction(chunk, (size_t)add_index);
    assert(VM_DECODE_R_K(add) == 1);
    assert(VM_DECODE_R_R2(add) == 1);

    test_program_free(&program);
}

static void test_a_float_literal_is_never_an_immediate() {
    TestProgram program = test_compile("let a: float = 10.0;\n"
                                       "let b: float = a + 1.0;\n");

    Chunk *chunk = test_top_chunk(&program);

    long add_index = test_find_opcode(chunk, OP_ADDFK);
    assert(add_index >= 0);
    assert(VM_DECODE_R_K(test_instruction(chunk, (size_t)add_index)) == 0);

    assert(chunk->const_pool->count == 2);

    test_program_free(&program);
}

static void test_a_temporary_register_is_reused() {
    TestProgram program = test_compile("func g(): int { return 1; }\n"
                                       "func f() {\n"
                                       "    let x: int = 0;\n"
                                       "    x = g() + 1;\n"
                                       "    x = g() + 2;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 1);

    long first = test_find_opcode(chunk, OP_CALL);
    assert(first >= 0);

    long second = -1;
    for (size_t i = (size_t)first + 1; i < chunk->instructions.size; i++) {
        if (VM_DECODE_OPCODE(test_instruction(chunk, i)) == OP_CALL) {
            second = (long)i;
            break;
        }
    }

    assert(second >= 0);
    assert(VM_DECODE_R_RD(test_instruction(chunk, (size_t)first)) ==
           VM_DECODE_R_RD(test_instruction(chunk, (size_t)second)));

    test_program_free(&program);
}

static void test_assignment_computes_into_its_target() {
    TestProgram program = test_compile("func f() {\n"
                                       "    let x: int = 1;\n"
                                       "    x = x + 1;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_MOVE) == 0);
    assert(test_count_opcode(chunk, OP_ADDI) == 1);

    long add_index = test_find_opcode(chunk, OP_ADDI);
    Instruction add = test_instruction(chunk, (size_t)add_index);

    assert(VM_DECODE_R_RD(add) == VM_DECODE_R_R1(add));

    test_program_free(&program);
}

static void test_if_jumps_past_its_then_block() {
    TestProgram program = test_compile("func f() {\n"
                                       "    let a: int = 1;\n"
                                       "    if a > 0 { let b: int = 2; }\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    long jump_index = test_find_opcode(chunk, OP_JMP_IF_FALSE);
    assert(jump_index >= 0);

    Instruction jump = test_instruction(chunk, (size_t)jump_index);

    long cmp_index = test_find_opcode(chunk, OP_CMP_GTI);
    assert(cmp_index >= 0);
    assert(cmp_index < jump_index);
    assert(VM_DECODE_I_RD(jump) == VM_DECODE_R_RD(test_instruction(chunk, (size_t)cmp_index)));

    unsigned int offset = VM_DECODE_I_IMM(jump);
    assert(offset > 0);
    assert((size_t)jump_index + 1 + offset <= chunk->instructions.size);

    assert(test_count_opcode(chunk, OP_JMP) == 0);

    test_program_free(&program);
}

static void test_if_else_jumps_over_the_else_block() {
    TestProgram program = test_compile("func f() {\n"
                                       "    let a: int = 1;\n"
                                       "    if a > 0 { let b: int = 2; } else { let c: int = 3; }\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_JMP_IF_FALSE) == 1);
    assert(test_count_opcode(chunk, OP_JMP) == 1);

    long conditional = test_find_opcode(chunk, OP_JMP_IF_FALSE);
    long unconditional = test_find_opcode(chunk, OP_JMP);

    assert(conditional < unconditional);

    unsigned int offset = VM_DECODE_I_IMM(test_instruction(chunk, (size_t)unconditional));
    assert((size_t)unconditional + 1 + offset <= chunk->instructions.size);

    test_program_free(&program);
}

static void test_one_instantiation_serves_every_call_that_names_it() {
    TestProgram program = test_compile("func id<T>(x: T): T { return x; }\n"
                                       "func f(): int { return id<int>(1) + id<int>(2); }\n"
                                       "let r: int = f();");

    assert(test_func_count(&program) == 3);

    test_program_free(&program);
}

static void test_a_function_compiles_into_its_own_chunk() {
    TestProgram program = test_compile("func add(a: int, b: int): int { return a + b; }\n");

    assert(test_top_chunk(&program)->instructions.size == 1);
    assert(test_count_opcode(test_top_chunk(&program), OP_RETURN) == 1);

    assert(test_func_count(&program) == 1);
    assert(test_func_proto(&program, 0)->arity == 2);

    Chunk *body = test_func_chunk(&program, 0);

    assert(test_count_opcode(body, OP_ADDI) == 1);
    assert(test_count_opcode(body, OP_RETURN) == 1);

    long add_index = test_find_opcode(body, OP_ADDI);
    long ret_index = test_find_opcode(body, OP_RETURN);

    assert(add_index < ret_index);
    assert(VM_DECODE_R_R1(test_instruction(body, (size_t)ret_index)) ==
           VM_DECODE_R_RD(test_instruction(body, (size_t)add_index)));

    test_program_free(&program);
}

static void test_a_method_counts_its_receiver() {
    TestProgram program = test_compile("struct Point { x: int }\n"
                                       "func Point::scaled(v: &Point, by: int): int { return v.x * by; }\n");

    assert(test_func_count(&program) == 1);
    assert(test_func_proto(&program, 0)->arity == 2);

    test_program_free(&program);
}

static void test_a_struct_copy_is_one_instruction() {
    TestProgram program = test_compile("struct Point { x: int, y: int, z: int }\n"
                                       "func f() {\n"
                                       "    let a = Point { x: 0, y: 0, z: 0 };\n"
                                       "    let b: Point = a;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_MOVE_N) == 1);
    assert(test_count_opcode(chunk, OP_MOVE) == 0);

    long copy = test_find_opcode(chunk, OP_MOVE_N);
    assert(VM_DECODE_R_R2(test_instruction(chunk, (size_t)copy)) == 3);

    test_program_free(&program);
}

static void test_a_scalar_copy_stays_a_single_move() {
    TestProgram program = test_compile("func f(): int {\n"
                                       "    let x: int = 1;\n"
                                       "    let y: int = x;\n"
                                       "    return y;\n"
                                       "}\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_MOVE_N) == 0);

    test_program_free(&program);
}

static void test_a_pointer_copy_batches_when_it_is_wide() {
    TestProgram program = test_compile("func f(): int {\n"
                                       "    let x: int = 1;\n"
                                       "    let p: &int = x;\n"
                                       "    return *p;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    long addr = test_find_opcode(chunk, OP_ADDR_OF);
    assert(addr >= 0);

    Instruction copy = test_instruction(chunk, (size_t)addr + 1);

    if (VM_INDIRECT_SLOTS > 1) {
        assert(VM_DECODE_OPCODE(copy) == OP_MOVE_N);
        assert(VM_DECODE_R_R2(copy) == VM_INDIRECT_SLOTS);
    } else {
        assert(VM_DECODE_OPCODE(copy) == OP_MOVE);
    }

    assert(VM_DECODE_R_RD(copy) % VM_INDIRECT_SLOTS == 0);

    test_program_free(&program);
}

static void test_a_self_copy_emits_nothing() {
    TestProgram self = test_compile("struct Point { x: int, y: int }\n"
                                    "func f() {\n"
                                    "    let a = Point { x: 0, y: 0 };\n"
                                    "    a = a;\n"
                                    "}\n");

    Chunk *chunk = test_func_chunk(&self, 0);

    assert(test_count_opcode(chunk, OP_MOVE_N) == 0);
    assert(test_count_opcode(chunk, OP_MOVE) == 0);

    test_program_free(&self);
}

static void test_a_struct_read_through_a_pointer_is_one_instruction() {
    TestProgram program = test_compile("struct Point { x: int, y: int, z: int }\n"
                                       "func f() {\n"
                                       "    let a = Point { x: 0, y: 0, z: 0 };\n"
                                       "    let p: &Point = a;\n"
                                       "    let b: Point = *p;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_LOAD_PTR_N) == 1);

    long load = test_find_opcode(chunk, OP_LOAD_PTR_N);
    assert(VM_DECODE_R_R2(test_instruction(chunk, (size_t)load)) == 3);

    test_program_free(&program);
}

static void test_a_struct_write_through_a_pointer_is_one_instruction() {
    TestProgram program = test_compile("struct Point { x: int, y: int, z: int }\n"
                                       "func f() {\n"
                                       "    let a = Point { x: 0, y: 0, z: 0 };\n"
                                       "    let b = Point { x: 0, y: 0, z: 0 };\n"
                                       "    let p: &Point = a;\n"
                                       "    *p = b;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_STORE_PTR_N) == 1);

    long store = test_find_opcode(chunk, OP_STORE_PTR_N);
    assert(VM_DECODE_R_R2(test_instruction(chunk, (size_t)store)) == 3);

    test_program_free(&program);
}

static void test_a_release_names_what_it_frees() {
    TestProgram program = test_compile("struct Node { n: int }\n"
                                       "func f(): int {\n"
                                       "    let p: *Node = box Node { n: 0 };\n"
                                       "    return 0;\n"
                                       "}\n");
    Chunk *chunk = test_func_chunk(&program, 0);

    long released = test_first_operand(chunk, OP_RELEASE);

    assert(released >= 0);
    assert(released != test_first_operand(chunk, OP_BOX));

    const Type *type = program.vm->program.shape_types.data[released];

    assert(type_kind(type) == TYPE_BOX);
    assert(type_name_of(type_pointee(type)) && strcmp(type_name_of(type_pointee(type))->data, "Node") == 0);

    test_program_free(&program);
}

static void test_break_releases_what_the_body_owns() {
    TestProgram program = test_compile("struct Node { n: int }\n"
                                       "func f(): int {\n"
                                       "    for { let p: *Node = box Node { n: 0 }; break; }\n"
                                       "    return 0;\n"
                                       "}\n");
    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_BOX) == 1);
    assert(test_count_opcode(chunk, OP_RELEASE) == 2);

    test_program_free(&program);
}

static void test_a_float_literal_needs_no_load() {
    TestProgram program = test_compile("func f(): float { let x: float = 1.0; return x + 1.5; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_ADDFK) == 1);
    assert(test_count_opcode(chunk, OP_ADDF) == 0);

    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);

    test_program_free(&program);
}

static void test_a_float_literal_on_the_left_keeps_the_register_form() {
    TestProgram program = test_compile("func f(): float { let x: float = 1.0; return 1.5 - x; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_SUBFK) == 0);
    assert(test_count_opcode(chunk, OP_SUBF) == 1);

    test_program_free(&program);
}

static void test_an_int_literal_still_uses_the_immediate() {
    TestProgram program = test_compile("func f(): int { let x: int = 1; return x + 2; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    long add = test_find_opcode(chunk, OP_ADDI);
    assert(add >= 0);
    assert(VM_DECODE_R_K(test_instruction(chunk, (size_t)add)) == 1);

    test_program_free(&program);
}

static void test_float_constant_arithmetic_computes() {
    assert(test_run_float("func f(): float { let x: float = 2.0; return x + 1.5; }\n"
                          "let r: float = f();\n") == 3.5f);

    assert(test_run_float("func f(): float { let x: float = 2.0; return x - 1.5; }\n"
                          "let r: float = f();\n") == 0.5f);

    assert(test_run_float("func f(): float { let x: float = 2.0; return x * 1.5; }\n"
                          "let r: float = f();\n") == 3.0f);

    assert(test_run_float("func f(): float { let x: float = 3.0; return x / 1.5; }\n"
                          "let r: float = f();\n") == 2.0f);
}

static void test_the_constant_is_the_right_operand() {
    assert(test_run_float("func f(): float { let x: float = 10.0; return x - 4.0; }\n"
                          "let r: float = f();\n") == 6.0f);

    assert(test_run_float("func f(): float { let x: float = 10.0; return x / 4.0; }\n"
                          "let r: float = f();\n") == 2.5f);
}

static void test_a_float_compound_assignment_takes_the_constant() {
    TestProgram program = test_compile("func f(): float { let x: float = 1.0; x *= 2.5; return x; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_MULFK) == 1);

    test_program_free(&program);

    assert(test_run_float("func f(): float { let x: float = 4.0; x /= 2.0; return x; }\n"
                          "let r: float = f();\n") == 2.0f);
}

static void test_a_chunk_past_the_index_bound_falls_back() {
    char source[32768];
    size_t used = (size_t)snprintf(source, sizeof(source), "func f(): float {\n    let x: float = 0.0;\n");

    for (unsigned int i = 0; i < 300; i++) {
        used += (size_t)snprintf(source + used, sizeof(source) - used, "    x += %u.5;\n", i);
    }

    snprintf(source + used, sizeof(source) - used, "    return x;\n}\n");

    TestProgram program = test_compile(source);
    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_ADDFK) > 0);
    assert(test_count_opcode(chunk, OP_ADDF) > 0);

    test_program_free(&program);
}

static void test_box_encodes_the_type_index_the_vm_holds() {
    TestProgram program = test_compile(
        "module M;\n"
        "struct Wide { a: int, b: int, c: int, d: int }\n"
        "func first(): int { let p: *Wide = box Wide { a: 0, b: 0, c: 0, d: 0 }; return p.a; }\n");

    test_compile_next(&program, "module M;\n"
                                "struct Narrow { a: int }\n"
                                "func second(): int {\n"
                                "    let n: *Narrow = box Narrow { a: 0 };\n"
                                "    let w: *Wide = box Wide { a: 0, b: 0, c: 0, d: 0 };\n"
                                "    return n.a + w.a;\n"
                                "}\n");

    long narrow = test_heap_type_index(&program, "Narrow");
    long wide = test_heap_type_index(&program, "Wide");

    assert(narrow >= 0 && wide >= 0 && narrow != wide);

    assert(test_first_operand(test_func_chunk(&program, 1), OP_BOX) == narrow);

    test_program_free(&program);
}

static void test_a_signature_too_wide_for_a_frame_is_refused(void) {
    char source[8192];
    size_t at = 0;

    at += (size_t)snprintf(source + at, sizeof(source) - at, "module test;\nstruct Big { ");

    for (int i = 0; i < 7; i++) {
        at += (size_t)snprintf(source + at, sizeof(source) - at, "f%d: int, ", i);
    }

    at += (size_t)snprintf(source + at, sizeof(source) - at, "last: int }\nfunc fat(");

    for (int i = 0; i < 40; i++) {
        at += (size_t)snprintf(source + at, sizeof(source) - at, "%sp%d: Big", i ? ", " : "", i);
    }

    snprintf(source + at, sizeof(source) - at, ") { }\n");

    assert(!test_codegens(source));
}

static void test_every_chunk_ends_in_a_return() {
    TestProgram program = test_compile("func f(): int { return 1; }\n"
                                       "let a: int = 1;\n"
                                       "let b: int = f();\n");

    Chunk *top = test_top_chunk(&program);
    Chunk *body = test_func_chunk(&program, 0);

    assert(top->instructions.size > 0);
    assert(VM_DECODE_OPCODE(instruction_list_back(&top->instructions)) == OP_RETURN);

    assert(body->instructions.size > 0);
    assert(VM_DECODE_OPCODE(instruction_list_back(&body->instructions)) == OP_RETURN);

    test_program_free(&program);
}

static void test_every_jump_lands_inside_its_chunk() {
    TestProgram program = test_compile("func f(n: int): int {\n"
                                       "    let acc: int = 0;\n"
                                       "    for let i: int = 0; i < n; i += 1 {\n"
                                       "        if i > 3 { acc += i; } else { continue; }\n"
                                       "        if acc > 99 { break; }\n"
                                       "    }\n"
                                       "    for let j: int = 0; j < n; j += 2 { acc += j; }\n"
                                       "    return acc;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);
    size_t size = chunk->instructions.size;

    size_t jumps = 0;

    for (size_t at = 0; at < size; at++) {
        Instruction instruction = chunk->instructions.data[at];

        long target;

        switch (VM_DECODE_OPCODE(instruction)) {
        case OP_JMP:
        case OP_JMP_IF_FALSE:
        case OP_JMP_IF_TRUE:
            target = (long)at + 1 + VM_DECODE_I_SIMM(instruction);
            break;

        case OP_FOR_LOOP:
            target = (long)at + 1 - (long)VM_DECODE_R_BACK(instruction);
            break;

        default:
            continue;
        }

        jumps++;

        assert(target >= 0 && (size_t)target < size);
    }

    assert(jumps > 0);

    test_program_free(&program);
}

int main() {
    test_a_constant_expression_folds_to_one_load();
    test_a_constant_float_expression_folds();
    test_a_nested_constant_expression_folds_wholly();
    test_a_variable_operand_does_not_fold();
    test_a_constant_division_by_zero_does_not_fold();
    test_negated_literal_folds_to_one_load();
    test_negating_a_variable_emits_one_instruction();
    test_negating_a_float_variable_emits_one_instruction();
    test_a_float_compared_to_a_literal_uses_the_constant_form();
    test_a_binary_op_reads_its_operands();
    test_a_small_literal_becomes_an_immediate();
    test_a_compound_assignment_takes_an_immediate();
    test_a_float_literal_is_never_an_immediate();
    test_a_float_literal_needs_no_load();
    test_a_float_literal_on_the_left_keeps_the_register_form();
    test_an_int_literal_still_uses_the_immediate();
    test_float_constant_arithmetic_computes();
    test_the_constant_is_the_right_operand();
    test_a_float_compound_assignment_takes_the_constant();
    test_a_chunk_past_the_index_bound_falls_back();
    test_a_temporary_register_is_reused();
    test_assignment_computes_into_its_target();
    test_if_jumps_past_its_then_block();
    test_if_else_jumps_over_the_else_block();
    test_every_chunk_ends_in_a_return();
    test_every_jump_lands_inside_its_chunk();
    test_one_instantiation_serves_every_call_that_names_it();
    test_a_function_compiles_into_its_own_chunk();
    test_a_method_counts_its_receiver();
    test_a_release_names_what_it_frees();
    test_break_releases_what_the_body_owns();

    test_a_struct_copy_is_one_instruction();
    test_a_scalar_copy_stays_a_single_move();
    test_a_pointer_copy_batches_when_it_is_wide();
    test_a_self_copy_emits_nothing();
    test_a_struct_read_through_a_pointer_is_one_instruction();
    test_a_struct_write_through_a_pointer_is_one_instruction();

    test_box_encodes_the_type_index_the_vm_holds();

    test_a_signature_too_wide_for_a_frame_is_refused();

    printf("codegen_test: all tests passed\n");
    return 0;
}
