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
#include <string.h>

// An arithmetic operator over two literals is computed once, here, rather than
// once per execution. Everything a constant expression needs is known now, so
// leaving it for the VM spends an instruction each time round a loop.
static void test_a_constant_expression_folds_to_one_load() {
    TestProgram program = test_compile("let x: int = 2 + 3;\n");
    Chunk *chunk = test_top_chunk(&program);

    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);
    assert(test_count_opcode(chunk, OP_ADDI) == 0);

    assert(chunk->const_pool->count == 1);
    assert(chunk->const_pool->constants[0].as_int == 5);

    test_program_free(&program);
}

// The same for floats, which reach the pool rather than an immediate and so
// take a different path to the same place.
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

// Folding runs with resolution, so it happens bottom-up: an inner fold makes its
// parent foldable in turn, and a whole constant tree collapses to one load
// rather than only its innermost operator.
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

// Folding stops where a value is not known: an operand that is a variable makes
// the expression a runtime one however constant the other side is.
static void test_a_variable_operand_does_not_fold() {
    TestProgram program = test_compile("let a: int = 2;\n"
                                       "let x: int = a + 3;\n");

    assert(test_count_opcode(test_top_chunk(&program), OP_ADDI) == 1);

    test_program_free(&program);
}

// Division by zero has no value to fold to, so it stays an instruction and
// traps at runtime as it always did.
static void test_a_constant_division_by_zero_does_not_fold() {
    TestProgram program = test_compile("func f(): int { return 1 / 0; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_DIVI) == 1);

    test_program_free(&program);
}

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

// Negating anything else cannot fold, so it emits the instruction. One
// instruction and no constant: a subtraction from zero would need the zero
// loaded into a register first, since only a second operand may be immediate.
//
// Checked alongside the fold so that a change disabling the fold entirely would
// show up as one test failing rather than both passing vacuously.
static void test_negating_a_variable_emits_one_instruction() {
    TestProgram program = test_compile("let a: int = 42;\n"
                                       "let x: int = -a;\n");

    Chunk *chunk = test_top_chunk(&program);

    assert(test_count_opcode(chunk, OP_NEGI) == 1);
    assert(test_count_opcode(chunk, OP_SUBI) == 0);

    // Only the 42: the zero the old lowering needed is gone.
    assert(chunk->const_pool->count == 1);

    test_program_free(&program);
}

// The float operand takes its own opcode, so a change touching one type does
// not silently leave the other emitting a subtraction.
static void test_negating_a_float_variable_emits_one_instruction() {
    TestProgram program = test_compile("let a: float = 1.5;\n"
                                       "let x: float = -a;\n");

    Chunk *chunk = test_top_chunk(&program);

    assert(test_count_opcode(chunk, OP_NEGF) == 1);
    assert(test_count_opcode(chunk, OP_SUBF) == 0);

    test_program_free(&program);
}

// A float compared against a literal reads it from the constant pool rather
// than loading it into a register first, which is what 'p.y < 0.0' costs on
// every iteration of a loop without the K form.
static void test_a_float_compared_to_a_literal_uses_the_constant_form() {
    TestProgram program = test_compile("func f(a: float): bool { return a < 0.0; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_CMP_LTFK) == 1);
    assert(test_count_opcode(chunk, OP_CMP_LTF) == 0);

    // The literal reaches the instruction as a pool index, so the k bit stays
    // clear -- that bit means an inline integer.
    long at = test_find_opcode(chunk, OP_CMP_LTFK);
    assert(VM_DECODE_R_K(test_instruction(chunk, (size_t)at)) == 0);

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

// A float is never an immediate: the operand field is eight bits and reads as
// an integer. It reaches the instruction as a pool index instead, which is
// what the K form is for -- but the k bit stays clear either way.
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

// Each statement reclaims the registers it allocated above the locals, so two
// statements of the same shape reuse the same temporary rather than growing
// the frame.
//
// A call is what still needs a temporary: its result lands in a fresh register
// before anything consumes it, where a literal or a variable is generated
// straight into its destination.
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

    // The second call must land in the same register the first did, which is
    // what reclaiming between statements buys.
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

// 'x = x + 1' computes straight into x's register instead of into a temporary
// that is then moved. The saved move is the whole point, so its absence is the
// assertion.
static void test_assignment_computes_into_its_target() {
    TestProgram program = test_compile("func f() {\n"
                                       "    let x: int = 1;\n"
                                       "    x = x + 1;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    // No move at all: the initialiser loads into x, and the sum computes into
    // it, so nothing is ever staged elsewhere and copied down.
    assert(test_count_opcode(chunk, OP_MOVE) == 0);
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

// A function compiles into its own chunk, leaving nothing in the script's but
// the return every chunk ends with.
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

// A method takes its receiver as parameter zero, so its arity is one more than
// the parameters it declares.
static void test_a_method_counts_its_receiver() {
    TestProgram program = test_compile("struct Vec { x: int }\n"
                                       "func (v: ref Vec) scaled(by: int): int { return v.x * by; }\n");

    assert(test_func_count(&program) == 1);
    assert(test_func_proto(&program, 0)->arity == 2);

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
// reason a struct does. Written in terms of VM_INDIRECT_SLOTS rather than 2, so
// the claim still reads correctly on a host where a pointer is one slot.
static void test_a_pointer_copy_batches_when_it_is_wide() {
    TestProgram program = test_compile("func f(): int {\n"
                                       "    let x: int = 1;\n"
                                       "    let p: ref int = x;\n"
                                       "    return *p;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    // '&x' lands in a temporary and is then copied into p's slot.
    long addr = test_find_opcode(chunk, OP_ADDR_OF);
    assert(addr >= 0);

    Instruction copy = test_instruction(chunk, (size_t)addr + 1);

    if (VM_INDIRECT_SLOTS > 1) {
        assert(VM_DECODE_OPCODE(copy) == OP_MOVE_N);
        assert(VM_DECODE_R_R2(copy) == VM_INDIRECT_SLOTS);
    } else {
        assert(VM_DECODE_OPCODE(copy) == OP_MOVE);
    }

    // An 8-byte pointer needs an even slot index to sit at its natural
    // alignment. The odd leading scalar is what would push it off.
    assert(VM_DECODE_R_RD(copy) % VM_INDIRECT_SLOTS == 0);

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
                                       "    let p: ref Vec = a;\n"
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
                                       "    let p: ref Vec = a;\n"
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
// A release names the type it frees, so the drop it runs is the one chosen when
// that type's layout was computed rather than one rediscovered from the object.
// That is what lets a value whose bounds are not in its type -- an array, whose
// length sits beside its pointer -- be freed by the same instruction as a
// pointer.
static void test_a_release_names_what_it_frees() {
    TestProgram program = test_compile("struct Node { n: int }\n"
                                       "func f(): int {\n"
                                       "    let p: box Node = new Node;\n"
                                       "    return 0;\n"
                                       "}\n");
    Chunk *chunk = test_func_chunk(&program, 0);

    // The slot's own type, which is the 'box Node' holding the object rather
    // than the 'Node' that was allocated: what a release frees is what the slot
    // holds, and following the pointer is that type's drop.
    long released = test_first_operand(chunk, OP_RELEASE);

    assert(released >= 0);
    assert(released != test_first_operand(chunk, OP_NEW));

    // An owning indirection to the struct that was allocated: freeing the slot
    // follows the pointer, which is what 'box Node's drop does.
    const Type *type = program.vm->program.heap_types.data[released];

    assert(type->kind == TYPE_INDIRECT && !type->is_ref);
    assert(type->inner->name && strcmp(type->inner->name->data, "Node") == 0);

    test_program_free(&program);
}

static void test_break_releases_what_the_body_owns() {
    TestProgram program = test_compile("struct Node { n: int }\n"
                                       "func f(): int {\n"
                                       "    for { let p: box Node = new Node; break; }\n"
                                       "    return 0;\n"
                                       "}\n");
    Chunk *chunk = test_func_chunk(&program, 0);

    // One for the break's own exit, one for the close of the body block on the
    // path that falls through to it.
    assert(test_count_opcode(chunk, OP_NEW) == 1);
    assert(test_count_opcode(chunk, OP_RELEASE) == 2);

    test_program_free(&program);
}

// A float literal on the right of an arithmetic operator is read from the
// constant pool by the instruction itself. Without this every such operation
// costs a load of its own, because the 8-bit immediate field holds an integer
// and no float has an eight-bit encoding.
static void test_a_float_literal_needs_no_load() {
    TestProgram program = test_compile("func f(): float { let x: float = 1.0; return x + 1.5; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_ADDFK) == 1);
    assert(test_count_opcode(chunk, OP_ADDF) == 0);

    // Only the initialiser's own constant is loaded; the 1.5 rides in the add.
    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);

    test_program_free(&program);
}

// The constant is the right operand only. 'literal - x' cannot use the form,
// since subtraction does not commute and the instruction reads its left
// operand from a register.
static void test_a_float_literal_on_the_left_keeps_the_register_form() {
    TestProgram program = test_compile("func f(): float { let x: float = 1.0; return 1.5 - x; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_SUBFK) == 0);
    assert(test_count_opcode(chunk, OP_SUBF) == 1);

    test_program_free(&program);
}

// Integers keep the immediate form, which costs no pool entry at all.
static void test_an_int_literal_still_uses_the_immediate() {
    TestProgram program = test_compile("func f(): int { let x: int = 1; return x + 2; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    long add = test_find_opcode(chunk, OP_ADDI);
    assert(add >= 0);
    assert(VM_DECODE_R_K(test_instruction(chunk, (size_t)add)) == 1);

    test_program_free(&program);
}

// The four arithmetic operators, and the values they must still produce.
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

// Subtraction and division do not commute, so these are what catch an
// implementation that read the operands the other way round.
static void test_the_constant_is_the_right_operand() {
    assert(test_run_float("func f(): float { let x: float = 10.0; return x - 4.0; }\n"
                          "let r: float = f();\n") == 6.0f);

    assert(test_run_float("func f(): float { let x: float = 10.0; return x / 4.0; }\n"
                          "let r: float = f();\n") == 2.5f);
}

// A compound assignment assigns the result of the same operator, so it takes
// the same form.
static void test_a_float_compound_assignment_takes_the_constant() {
    TestProgram program = test_compile("func f(): float { let x: float = 1.0; x *= 2.5; return x; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_MULFK) == 1);

    test_program_free(&program);

    assert(test_run_float("func f(): float { let x: float = 4.0; x /= 2.0; return x; }\n"
                          "let r: float = f();\n") == 2.0f);
}

// Past the 256th constant the index no longer fits the operand field, so those
// operations fall back to loading the value into a register. The fallback is
// what keeps the bound an optimisation rather than a limit on what compiles.
static void test_a_chunk_past_the_index_bound_falls_back() {
    char source[32768];
    size_t used = (size_t)snprintf(source, sizeof(source), "func f(): float {\n    let x: float = 0.0;\n");

    // Each literal is distinct, so each takes a pool entry of its own; the
    // pool deduplicates, and repeating one would never grow it.
    for (unsigned int i = 0; i < 300; i++) {
        used += (size_t)snprintf(source + used, sizeof(source) - used, "    x += %u.5;\n", i);
    }

    snprintf(source + used, sizeof(source) - used, "    return x;\n}\n");

    TestProgram program = test_compile(source);
    Chunk *chunk = test_func_chunk(&program, 0);

    // The first 256 ride in the instruction; the rest are loaded.
    assert(test_count_opcode(chunk, OP_ADDFK) > 0);
    assert(test_count_opcode(chunk, OP_ADDF) > 0);

    test_program_free(&program);
}

// A second unit numbers its types from zero, so the operand it emits for a type
// the VM already holds is not the index that type sits at until linking rewrites
// it. Behaviour cannot see this: the wrong type still allocates, just at the
// wrong size, and nothing a program can read reports a size.
static void test_new_encodes_the_type_index_the_vm_holds() {
    TestProgram program = test_compile("module M;\n"
                                       "struct Wide { a: int, b: int, c: int, d: int }\n"
                                       "func first(): int { let p: box Wide = new Wide; return p.a; }\n");

    // Its own type first, so this unit numbers 'Wide' second while the VM
    // already holds it first: the two orderings disagree, and the operand is
    // only right if it was rewritten.
    test_compile_next(&program, "module M;\n"
                                "struct Narrow { a: int }\n"
                                "func second(): int {\n"
                                "    let n: box Narrow = new Narrow;\n"
                                "    let w: box Wide = new Wide;\n"
                                "    return n.a + w.a;\n"
                                "}\n");

    long narrow = test_heap_type_index(&program, "Narrow");
    long wide = test_heap_type_index(&program, "Wide");

    assert(narrow >= 0 && wide >= 0 && narrow != wide);

    // 'new Narrow' comes first in the body, so the first OP_NEW is the one to
    // ask -- and what it names must be where the VM put 'Narrow'.
    assert(test_first_operand(test_func_chunk(&program, 1), OP_NEW) == narrow);

    test_program_free(&program);
}

// Parameters are placed at fixed slots rather than allocated, so the bound the
// allocator enforces does not reach them. A signature wide enough to run past
// the frame is refused: a struct parameter occupies its whole width, so a
// plausible-looking parameter list can still ask for more slots than an operand
// can name.
static void test_a_signature_too_wide_for_a_frame_is_refused(void) {
    char source[8192];
    size_t at = 0;

    at += (size_t)snprintf(source + at, sizeof(source) - at, "module test;\nstruct Big { ");

    for (int i = 0; i < 7; i++) {
        at += (size_t)snprintf(source + at, sizeof(source) - at, "f%d: int, ", i);
    }

    at += (size_t)snprintf(source + at, sizeof(source) - at, "last: int }\nfunc fat(");

    // Eight slots each, and enough of them to pass what a frame addresses. The
    // body allocates nothing, so nothing but the signature can trip the bound.
    for (int i = 0; i < 40; i++) {
        at += (size_t)snprintf(source + at, sizeof(source) - at, "%sp%d: Big", i ? ", " : "", i);
    }

    snprintf(source + at, sizeof(source) - at, ") { }\n");

    assert(!test_codegens(source));
}
// Every chunk ends in a return, the top level included. The interpreter reads
// that as its guarantee that stepping one instruction stays inside the chunk,
// so a chunk ending any other way would run off the end of it.
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

// Allocating a block names no type: how many bytes and at what alignment is
// everything the instruction says, so one opcode serves an array of any
// element and would serve any other collection. Nothing about it is relocated,
// which is what a type index would have required.
static void test_allocating_a_block_names_no_type() {
    TestProgram program = test_compile("func f(): int {\n"
                                       "    let xs: Array int = Array int[3];\n"
                                       "    return 0;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_ALLOC) == 1);

    // The element's width rides in the immediate, so the byte count is computed
    // from the length rather than read from a type the instruction names.
    Instruction alloc = test_instruction(chunk, (size_t)test_find_opcode(chunk, OP_ALLOC));

    assert(VM_DECODE_R_K(alloc) == 1);
    assert(VM_DECODE_R_R2(alloc) == sizeof(int32_t));

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

    test_new_encodes_the_type_index_the_vm_holds();
    test_allocating_a_block_names_no_type();

    test_a_signature_too_wide_for_a_frame_is_refused();

    printf("codegen_test: all tests passed\n");
    return 0;
}
