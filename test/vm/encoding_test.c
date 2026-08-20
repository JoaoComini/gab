#include "vm/opcode.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Every field at its maximum at once. If any field bleeds into a neighbour it
// shows up here rather than as an instruction that matches no case at runtime.
static void test_r_type_round_trips_at_maximum() {
    Instruction instr = VM_ENCODE_RK(0x7F, VM_MAX_REGISTERS, VM_MAX_REGISTERS, VM_MAX_REGISTERS, 1);

    assert(VM_DECODE_OPCODE(instr) == 0x7F);
    assert(VM_DECODE_R_RD(instr) == VM_MAX_REGISTERS);
    assert(VM_DECODE_R_R1(instr) == VM_MAX_REGISTERS);
    assert(VM_DECODE_R_R2(instr) == VM_MAX_REGISTERS);
    assert(VM_DECODE_R_K(instr) == 1);

    // Nothing is left over: the four fields plus k account for all 32 bits.
    assert(instr == 0xFFFFFFFFu);
}

// One field at a time at maximum, everything else zero: a field that reaches
// past its own bits shows up as a non-zero neighbour.
static void test_r_type_fields_do_not_bleed() {
    Instruction rd = VM_ENCODE_R(0, VM_MAX_REGISTERS, 0, 0);
    assert(VM_DECODE_R_RD(rd) == VM_MAX_REGISTERS);
    assert(VM_DECODE_OPCODE(rd) == 0 && VM_DECODE_R_R1(rd) == 0 && VM_DECODE_R_R2(rd) == 0);
    assert(VM_DECODE_R_K(rd) == 0);

    Instruction r1 = VM_ENCODE_R(0, 0, VM_MAX_REGISTERS, 0);
    assert(VM_DECODE_R_R1(r1) == VM_MAX_REGISTERS);
    assert(VM_DECODE_OPCODE(r1) == 0 && VM_DECODE_R_RD(r1) == 0 && VM_DECODE_R_R2(r1) == 0);

    Instruction r2 = VM_ENCODE_R(0, 0, 0, VM_MAX_REGISTERS);
    assert(VM_DECODE_R_R2(r2) == VM_MAX_REGISTERS);
    assert(VM_DECODE_OPCODE(r2) == 0 && VM_DECODE_R_RD(r2) == 0 && VM_DECODE_R_R1(r2) == 0);

    Instruction op = VM_ENCODE_R(0x7F, 0, 0, 0);
    assert(VM_DECODE_OPCODE(op) == 0x7F);
    assert(VM_DECODE_R_RD(op) == 0 && VM_DECODE_R_R1(op) == 0 && VM_DECODE_R_R2(op) == 0);

    // The spare bit is the low bit and must not be disturbed by any operand.
    assert(VM_DECODE_R_K(r1) == 0 && VM_DECODE_R_K(r2) == 0 && VM_DECODE_R_K(op) == 0);
}

static void test_i_type_round_trips_at_maximum() {
    Instruction instr = VM_ENCODE_I(0x7F, VM_MAX_REGISTERS, VM_MAX_CONSTANTS);

    assert(VM_DECODE_OPCODE(instr) == 0x7F);
    assert(VM_DECODE_I_RD(instr) == VM_MAX_REGISTERS);
    assert(VM_DECODE_I_KX(instr) == VM_MAX_CONSTANTS);
    assert(VM_DECODE_I_IMM(instr) == VM_MAX_CONSTANTS);

    assert(instr == 0xFFFFFFFFu);
}

static void test_i_type_fields_do_not_bleed() {
    Instruction rd = VM_ENCODE_I(0, VM_MAX_REGISTERS, 0);
    assert(VM_DECODE_I_RD(rd) == VM_MAX_REGISTERS);
    assert(VM_DECODE_OPCODE(rd) == 0 && VM_DECODE_I_KX(rd) == 0);

    Instruction kx = VM_ENCODE_I(0, 0, VM_MAX_CONSTANTS);
    assert(VM_DECODE_I_KX(kx) == VM_MAX_CONSTANTS);
    assert(VM_DECODE_OPCODE(kx) == 0 && VM_DECODE_I_RD(kx) == 0);
}

// Every field is masked, so an out-of-range operand is truncated to its own
// field rather than smearing into the opcode's.
static void test_out_of_range_operands_stay_in_their_field() {
    Instruction instr = VM_ENCODE_R(OP_MOVE, VM_MAX_REGISTERS + 1, VM_MAX_REGISTERS + 3, 0);

    assert(VM_DECODE_OPCODE(instr) == OP_MOVE);
    assert(VM_DECODE_R_RD(instr) == 0);
    assert(VM_DECODE_R_R1(instr) == 2);

    Instruction wide = VM_ENCODE_I(OP_LOAD_CONST, 0, VM_MAX_CONSTANTS + 1);

    assert(VM_DECODE_OPCODE(wide) == OP_LOAD_CONST);
    assert(VM_DECODE_I_KX(wide) == 0);
}

// Masking alone would silently miscompile, so codegen rejects an over-large
// frame before it ever reaches an encode macro.
static void test_emit_site_rejects_an_over_large_frame() {
    VM *vm = vm_create();

    // 254 locals need more slots than the register field can address. One
    // register each: 'n + 1' takes the literal as an immediate operand and is
    // generated straight into the variable's slot, so neither the constant nor
    // the sum needs a register of its own.
    char source[8192];
    size_t used = (size_t)snprintf(source, sizeof(source), "func f(n: int): int {\n");

    for (unsigned int i = 0; i < 254; i++) {
        used += (size_t)snprintf(source + used, sizeof(source) - used, "let s%u = n + 1;\n", i);
    }

    snprintf(source + used, sizeof(source) - used, "return n;\n}\nlet r: int = f(7);\n");

    vm_execute(vm, source);

    // The program never ran, so the result slot keeps its zeroed value.
    int32_t returned;
    memcpy(&returned, vm_slot_at(vm, 0), sizeof(returned));

    assert(returned == 0);

    vm_free(vm);
}

// Every opcode must fit the 7-bit field, or its case becomes unreachable.
static void test_every_opcode_fits_the_field() {
    assert(OP_STORE_FIELD_4 <= 0x7F);

    Instruction instr = VM_ENCODE_R(OP_STORE_FIELD_4, 1, 2, 3);
    assert(VM_DECODE_OPCODE(instr) == OP_STORE_FIELD_4);
}

// A jump offset is the one I-type operand that carries a sign, so the same 17
// bits read one way as an index and another as a displacement.
static void test_a_jump_offset_round_trips_in_both_directions() {
    assert(VM_DECODE_I_SIMM(VM_ENCODE_I(OP_JMP, 0, 1)) == 1);
    assert(VM_DECODE_I_SIMM(VM_ENCODE_I(OP_JMP, 0, -1)) == -1);
    assert(VM_DECODE_I_SIMM(VM_ENCODE_I(OP_JMP, 0, 0)) == 0);

    assert(VM_DECODE_I_SIMM(VM_ENCODE_I(OP_JMP, 0, VM_MAX_JUMP)) == VM_MAX_JUMP);
    assert(VM_DECODE_I_SIMM(VM_ENCODE_I(OP_JMP, 0, -VM_MAX_JUMP)) == -VM_MAX_JUMP);

    // A negative offset fills the field's high bits, which must stay out of the
    // register and opcode fields above it.
    Instruction back = VM_ENCODE_I(OP_JMP, VM_MAX_REGISTERS, -1);
    assert(VM_DECODE_OPCODE(back) == OP_JMP);
    assert(VM_DECODE_I_RD(back) == VM_MAX_REGISTERS);
}

int main() {
    test_r_type_round_trips_at_maximum();
    test_r_type_fields_do_not_bleed();
    test_i_type_round_trips_at_maximum();
    test_i_type_fields_do_not_bleed();
    test_a_jump_offset_round_trips_in_both_directions();
    test_out_of_range_operands_stay_in_their_field();
    test_emit_site_rejects_an_over_large_frame();
    test_every_opcode_fits_the_field();

    printf("encoding_test: all tests passed\n");

    return 0;
}
