// What codegen emits, as opposed to what the emitted code computes.
//
// Only claims that behaviour cannot make belong here: the optimizations. A
// folded constant, a reclaimed register, a copy that stays one instruction --
// the program computes the same answer either way, so the instructions are the
// only place the optimization is visible.
//
// Which opcode an operator emits is not such a claim. That is a detail no
// program observes, and the behaviour tests in arithmetic_test.c and
// compare_test.c cover what it is meant to achieve.
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

    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);
    assert(test_count_opcode(chunk, OP_SUBI) == 0);

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

    Instruction add = test_instruction(chunk, (size_t)add_index);
    assert(VM_DECODE_R_K(add) == 1);
    assert(VM_DECODE_R_R2(add) == 1);

    assert(chunk->const_pool->count == 1);

    test_program_free(&program);
}

// A float has no immediate form, so the same shape must load its operand.
// A compound assignment takes the same immediate as the operator it assigns
// with: 'a += 1' is 'a + 1' stored back, so a small literal rides in the
// instruction rather than costing a load of its own each time round a loop.
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

    long add_index = test_find_opcode(chunk, OP_ADDF);
    assert(add_index >= 0);
    assert(VM_DECODE_R_K(test_instruction(chunk, (size_t)add_index)) == 0);

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

    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 2);
    assert(test_count_opcode(chunk, OP_MOVE) == 2);

    Instruction first_move = test_instruction(chunk, 1);
    Instruction second_move = test_instruction(chunk, 3);

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

    assert(test_count_opcode(chunk, OP_MOVE) == 1);
    assert(test_count_opcode(chunk, OP_ADDI) == 1);

    long add_index = test_find_opcode(chunk, OP_ADDI);
    Instruction add = test_instruction(chunk, (size_t)add_index);

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

    assert(conditional < unconditional);

    unsigned int offset = VM_DECODE_I_IMM(test_instruction(chunk, (size_t)unconditional));
    assert((size_t)unconditional + 1 + offset <= chunk->instructions.size);

    test_program_free(&program);
}

// A function compiles into its own chunk, leaving nothing in the script's.
static void test_a_function_compiles_into_its_own_chunk() {
    TestProgram program = test_compile("func add(a: int, b: int): int { return a + b; }\n");

    assert(test_top_chunk(&program)->instructions.size == 0);

    assert(program.vm->global_funcs.size == 1);
    assert(program.vm->global_funcs.data[0].arity == 2);

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

// A method takes its receiver as parameter zero, so its arity is one more than
// the parameters it declares.
static void test_a_method_counts_its_receiver() {
    TestProgram program = test_compile("struct Vec { x: int }\n"
                                       "func (v: ref Vec) scaled(by: int): int { return v.x * by; }\n");

    assert(program.vm->global_funcs.size == 1);
    assert(program.vm->global_funcs.data[0].arity == 2);

    test_program_free(&program);
}

// A multi-slot value copies in one instruction. The interpreter's real cost is
// the dispatch, not the four bytes a slot move writes, so a struct of N slots
// must not become N moves.
static void test_a_struct_copy_is_one_instruction() {
    TestProgram program = test_compile("struct Vec { x: int, y: int, z: int }\n"
                                       "func f() {\n"
                                       "    let a: Vec;\n"
                                       "    let b: Vec = a;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_MOVE_N) == 1);
    assert(test_count_opcode(chunk, OP_MOVE) == 0);

    long copy = test_find_opcode(chunk, OP_MOVE_N);
    assert(VM_DECODE_R_R2(test_instruction(chunk, (size_t)copy)) == 3);

    test_program_free(&program);
}

// The property the batching must not cost: a one-slot copy stays OP_MOVE.
// OP_MOVE_N decodes a third operand and calls memmove, which is more work than
// the single assignment a scalar needs -- widening every move would slow the
// common case to speed the rare one.
static void test_a_scalar_copy_stays_a_single_move() {
    TestProgram program = test_compile("func f(): int {\n"
                                       "    let x: int = 1;\n"
                                       "    let y: int = x;\n"
                                       "    return y;\n"
                                       "}\n");

    // Nothing in a function of only int locals is wide enough to batch.
    assert(test_count_opcode(test_func_chunk(&program, 0), OP_MOVE_N) == 0);

    test_program_free(&program);
}

// A pointer is two slots on a 64-bit host, so copying one batches for the same
// reason a struct does. Written in terms of VM_POINTER_SLOTS rather than 2, so
// the claim still reads correctly on a host where a pointer is one slot.
static void test_a_pointer_copy_batches_when_it_is_wide() {
    TestProgram program = test_compile("func f(): int {\n"
                                       "    let x: int = 1;\n"
                                       "    let p: ref int = &x;\n"
                                       "    return *p;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    // '&x' lands in a temporary and is then copied into p's slot.
    long addr = test_find_opcode(chunk, OP_ADDR_OF);
    assert(addr >= 0);

    Instruction copy = test_instruction(chunk, (size_t)addr + 1);

    if (VM_POINTER_SLOTS > 1) {
        assert(VM_DECODE_OPCODE(copy) == OP_MOVE_N);
        assert(VM_DECODE_R_R2(copy) == VM_POINTER_SLOTS);
    } else {
        assert(VM_DECODE_OPCODE(copy) == OP_MOVE);
    }

    // An 8-byte pointer needs an even slot index to sit at its natural
    // alignment. The odd leading scalar is what would push it off.
    assert(VM_DECODE_R_RD(copy) % VM_POINTER_SLOTS == 0);

    test_program_free(&program);
}

// Copying a slot onto itself emits nothing at all: the guard is what keeps
// 'x = x' from spending an instruction to achieve nothing.
static void test_a_self_copy_emits_nothing() {
    TestProgram self = test_compile("struct Vec { x: int, y: int }\n"
                                    "func f() {\n"
                                    "    let a: Vec;\n"
                                    "    a = a;\n"
                                    "}\n");

    Chunk *chunk = test_func_chunk(&self, 0);

    assert(test_count_opcode(chunk, OP_MOVE_N) == 0);
    assert(test_count_opcode(chunk, OP_MOVE) == 0);

    test_program_free(&self);
}

// Reading a whole struct through a pointer is the indirect counterpart, and
// batches on the same argument: one OP_LOAD_PTR_N carrying the slot count
// rather than a load per slot.
static void test_a_struct_read_through_a_pointer_is_one_instruction() {
    TestProgram program = test_compile("struct Vec { x: int, y: int, z: int }\n"
                                       "func f() {\n"
                                       "    let a: Vec;\n"
                                       "    let p: ref Vec = &a;\n"
                                       "    let b: Vec = *p;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_LOAD_PTR_N) == 1);

    long load = test_find_opcode(chunk, OP_LOAD_PTR_N);
    assert(VM_DECODE_R_R2(test_instruction(chunk, (size_t)load)) == 3);

    test_program_free(&program);
}

// And writing one back, which is the store side of the same encoding.
static void test_a_struct_write_through_a_pointer_is_one_instruction() {
    TestProgram program = test_compile("struct Vec { x: int, y: int, z: int }\n"
                                       "func f() {\n"
                                       "    let a: Vec;\n"
                                       "    let b: Vec;\n"
                                       "    let p: ref Vec = &a;\n"
                                       "    *p = b;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_STORE_PTR_N) == 1);

    long store = test_find_opcode(chunk, OP_STORE_PTR_N);
    assert(VM_DECODE_R_R2(test_instruction(chunk, (size_t)store)) == 3);

    test_program_free(&program);
}

// A 'break' leaves the loop body without running the block's ordinary close, so
// it has to free what that block owns on the way out. The leak is invisible to
// a returned value, which is why the claim is made against the instructions.
static void test_break_releases_what_the_body_owns() {
    TestProgram program = test_compile("struct Node { n: int }\n"
                                       "func f(): int {\n"
                                       "    for { let p: *Node = new Node; break; }\n"
                                       "    return 0;\n"
                                       "}\n");
    Chunk *chunk = test_func_chunk(&program, 0);

    // One for the break's own exit, one for the close of the body block on the
    // path that falls through to it.
    assert(test_count_opcode(chunk, OP_NEW) == 1);
    assert(test_count_opcode(chunk, OP_RELEASE) == 2);

    test_program_free(&program);
}

int main() {
    test_negated_literal_folds_to_one_load();
    test_negating_a_variable_emits_a_subtraction();
    test_a_binary_op_reads_its_operands();
    test_a_small_literal_becomes_an_immediate();
    test_a_compound_assignment_takes_an_immediate();
    test_a_float_literal_is_never_an_immediate();
    test_a_temporary_register_is_reused();
    test_assignment_computes_into_its_target();
    test_if_jumps_past_its_then_block();
    test_if_else_jumps_over_the_else_block();
    test_a_function_compiles_into_its_own_chunk();
    test_a_method_counts_its_receiver();
    test_break_releases_what_the_body_owns();

    test_a_struct_copy_is_one_instruction();
    test_a_scalar_copy_stays_a_single_move();
    test_a_pointer_copy_batches_when_it_is_wide();
    test_a_self_copy_emits_nothing();
    test_a_struct_read_through_a_pointer_is_one_instruction();
    test_a_struct_write_through_a_pointer_is_one_instruction();

    printf("codegen_test: all tests passed\n");
    return 0;
}
