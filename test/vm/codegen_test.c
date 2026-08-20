// What codegen emits, as opposed to what the emitted code computes.
//
// The distinction decides what belongs here. A test asserting "'a + b' emits
// OP_ADDI" is checking a detail no program can observe -- and it passed
// happily while OP_CMP_GE dispatched to the wrong helper, because emitting the
// right opcode and running it correctly are different claims. Those tests are
// gone; arithmetic_test.c and compare_test.c check the behaviour instead.
//
// What is left is the claims behaviour genuinely cannot make: the
// optimizations. A folded constant, a reclaimed register, a copy that stays
// one instruction -- the program computes the same answer either way, so only
// the instructions show whether the optimization still happens.
//
// Registers are asserted relationally, never by number. Which register the
// allocator picks is its business; that one instruction reads what another
// wrote is the claim.
#include "support/run.h"

#include <assert.h>
#include <stdio.h>

// A literal negation folds into a single constant load rather than emitting a
// zero, a load, and a subtraction.
static void test_negated_literal_folds_to_one_load() {
    TestProgram program = test_compile("let x: int = -42;\n");
    Chunk *chunk = test_top_chunk(&program);

    // One load for the folded constant, one move into x's slot. No SUBI.
    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);
    assert(test_count_opcode(chunk, OP_SUBI) == 0);

    // The sign is in the pooled constant, not in an instruction.
    assert(chunk->const_pool->count == 1);
    assert(chunk->const_pool->constants[0].as_int == -42);

    test_program_free(&program);
}

// Negating anything else cannot fold, so it does emit the subtraction. Checked
// alongside the fold so that a change disabling the fold entirely would show
// up as one test failing rather than both passing vacuously.
static void test_negating_a_variable_emits_a_subtraction() {
    TestProgram program = test_compile("let a: int = 42;\n"
                                       "let x: int = -a;\n");

    assert(test_count_opcode(test_top_chunk(&program), OP_SUBI) == 1);

    test_program_free(&program);
}

// A binary op reads the two registers its operands were computed into. Which
// registers those are is the allocator's business; that the compare reads them
// both, and reads two distinct ones, is not.
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

    // Reading one register twice would compare a value with itself.
    assert(r1 != r2);

    test_program_free(&program);
}

// An integer literal small enough to ride in the instruction does, rather than
// being loaded into a register first. This is the k-bit immediate encoding,
// and it is invisible to the program: 'a + 1' computes the same either way.
static void test_a_small_literal_becomes_an_immediate() {
    TestProgram program = test_compile("let a: int = 10;\n"
                                       "let b: int = a + 1;\n");

    Chunk *chunk = test_top_chunk(&program);

    long add_index = test_find_opcode(chunk, OP_ADDI);
    assert(add_index >= 0);

    // The k bit is set, and the operand is the value itself rather than a
    // register holding it.
    Instruction add = test_instruction(chunk, (size_t)add_index);
    assert(VM_DECODE_R_K(add) == 1);
    assert(VM_DECODE_R_R2(add) == 1);

    // Only the 10 is pooled; the 1 never becomes a constant.
    assert(chunk->const_pool->count == 1);

    test_program_free(&program);
}

// A float has no immediate form, so the same shape must load its operand.
static void test_a_float_literal_is_never_an_immediate() {
    TestProgram program = test_compile("let a: float = 10.0;\n"
                                       "let b: float = a + 1.0;\n");

    Chunk *chunk = test_top_chunk(&program);

    long add_index = test_find_opcode(chunk, OP_ADDF);
    assert(add_index >= 0);
    assert(VM_DECODE_R_K(test_instruction(chunk, (size_t)add_index)) == 0);

    // Both floats are pooled, since neither can ride in the instruction.
    assert(chunk->const_pool->count == 2);

    test_program_free(&program);
}

// Each statement reclaims the registers it allocated above the locals, so two
// statements of the same shape reuse the same temporary rather than growing
// the frame.
static void test_a_temporary_register_is_reused() {
    TestProgram program = test_compile("func f() {\n"
                                       "    let x: float = 3.0;\n"
                                       "    x = 2.0;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    // Two loads, each into a temporary, each moved into x.
    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 2);
    assert(test_count_opcode(chunk, OP_MOVE) == 2);

    Instruction first_move = test_instruction(chunk, 1);
    Instruction second_move = test_instruction(chunk, 3);

    // Both moves write x's slot and read the same reclaimed temporary.
    assert(VM_DECODE_R_RD(first_move) == VM_DECODE_R_RD(second_move));
    assert(VM_DECODE_R_R1(first_move) == VM_DECODE_R_R1(second_move));

    test_program_free(&program);
}

// 'x = x + 1' computes straight into x's register instead of into a temporary
// that is then moved. The saved move is the whole point, so its absence is the
// assertion.
static void test_assignment_computes_into_its_target() {
    TestProgram program = test_compile("func f() {\n"
                                       "    let x: int = 1;\n"
                                       "    x = x + 1;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    // One move for the initializer. The compound assignment adds none.
    assert(test_count_opcode(chunk, OP_MOVE) == 1);
    assert(test_count_opcode(chunk, OP_ADDI) == 1);

    long add_index = test_find_opcode(chunk, OP_ADDI);
    Instruction add = test_instruction(chunk, (size_t)add_index);

    // The add writes the register it read, which is x's slot.
    assert(VM_DECODE_R_RD(add) == VM_DECODE_R_R1(add));

    test_program_free(&program);
}

// An 'if' jumps over its then-block when the condition is false. The offset is
// the allocator's arithmetic; that the jump lands past the block is the claim.
static void test_if_jumps_past_its_then_block() {
    TestProgram program = test_compile("func f() {\n"
                                       "    let a: int = 1;\n"
                                       "    if a > 0 { let b: int = 2; }\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    long jump_index = test_find_opcode(chunk, OP_JMP_IF_FALSE);
    assert(jump_index >= 0);

    Instruction jump = test_instruction(chunk, (size_t)jump_index);

    // The jump reads the register the comparison wrote.
    long cmp_index = test_find_opcode(chunk, OP_CMP_GTI);
    assert(cmp_index >= 0);
    assert(cmp_index < jump_index);
    assert(VM_DECODE_I_RD(jump) == VM_DECODE_R_RD(test_instruction(chunk, (size_t)cmp_index)));

    // It skips forward, and lands no further than the end of the chunk.
    unsigned int offset = VM_DECODE_I_IMM(jump);
    assert(offset > 0);
    assert((size_t)jump_index + 1 + offset <= chunk->instructions.size);

    // With no else, there is nothing to jump over on the way out.
    assert(test_count_opcode(chunk, OP_JMP) == 0);

    test_program_free(&program);
}

// An else-block needs a second jump: the then-block has to skip over it rather
// than falling into it.
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

    // The unconditional jump ends the then-block, so it comes after the test
    // and before the end.
    assert(conditional < unconditional);

    unsigned int offset = VM_DECODE_I_IMM(test_instruction(chunk, (size_t)unconditional));
    assert((size_t)unconditional + 1 + offset <= chunk->instructions.size);

    test_program_free(&program);
}

// A function compiles into its own chunk, leaving nothing in the script's.
static void test_a_function_compiles_into_its_own_chunk() {
    TestProgram program = test_compile("func add(a: int, b: int): int { return a + b; }\n");

    // The declaration emits no top-level code.
    assert(test_top_chunk(&program)->instructions.size == 0);

    assert(program.vm->global_funcs.size == 1);
    assert(program.vm->global_funcs.data[0].arity == 2);

    Chunk *body = test_func_chunk(&program, 0);

    assert(test_count_opcode(body, OP_ADDI) == 1);
    assert(test_count_opcode(body, OP_RETURN) == 1);

    // The return carries what the add produced.
    long add_index = test_find_opcode(body, OP_ADDI);
    long ret_index = test_find_opcode(body, OP_RETURN);

    assert(add_index < ret_index);
    assert(VM_DECODE_R_R1(test_instruction(body, (size_t)ret_index)) ==
           VM_DECODE_R_RD(test_instruction(body, (size_t)add_index)));

    test_program_free(&program);
}

// A method takes its receiver as parameter zero, so its arity is one more than
// the parameters it declares.
static void test_a_method_counts_its_receiver() {
    TestProgram program = test_compile("struct Vec { x: int }\n"
                                       "func (v: *Vec) scaled(by: int): int { return v.x * by; }\n");

    assert(program.vm->global_funcs.size == 1);
    assert(program.vm->global_funcs.data[0].arity == 2);

    test_program_free(&program);
}

int main() {
    test_negated_literal_folds_to_one_load();
    test_negating_a_variable_emits_a_subtraction();
    test_a_binary_op_reads_its_operands();
    test_a_small_literal_becomes_an_immediate();
    test_a_float_literal_is_never_an_immediate();
    test_a_temporary_register_is_reused();
    test_assignment_computes_into_its_target();
    test_if_jumps_past_its_then_block();
    test_if_else_jumps_over_the_else_block();
    test_a_function_compiles_into_its_own_chunk();
    test_a_method_counts_its_receiver();

    printf("codegen_test: all tests passed\n");
    return 0;
}
