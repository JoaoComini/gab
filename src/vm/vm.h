#ifndef GAB_VM_H
#define GAB_VM_H

#include "arena.h"
#include "scope.h"
#include "string/string_pool.h"
#include "util/list.h"
#include "value.h"
#include "vm/chunk.h"

#include <stdint.h>

/*
    Encodes R-type instructions in a 32-bit integer
    op: OpCode (7-bit)
    rd: Destination register (8-bit)
    r1: Register 1 (8-bit)
    r2: Register 2 (8-bit)
    k:  spare bit, reserved for Lua's register-or-constant operand trick
*/
#define VM_ENCODE_R(op, rd, r1, r2) VM_ENCODE_RK(op, rd, r1, r2, 0)

/*
    As VM_ENCODE_R, plus the spare k bit. Every field is masked: an
    out-of-range value would otherwise smear into its neighbours — including
    the opcode — and produce an instruction that matches no case.
*/
#define VM_ENCODE_RK(op, rd, r1, r2, k)                                                                      \
    ((((op) & 0x7F) << 25) | (((rd) & 0xFF) << 17) | (((r1) & 0xFF) << 9) | (((r2) & 0xFF) << 1) |           \
     ((k) & 0x1))

#define VM_DECODE_R_RD(instr) (((instr) >> 17) & 0xFF) // Destination register
#define VM_DECODE_R_R1(instr) (((instr) >> 9) & 0xFF)  // First source register
#define VM_DECODE_R_R2(instr) (((instr) >> 1) & 0xFF)  // Second source register
#define VM_DECODE_R_K(instr) ((instr) & 0x1)           // Spare bit, always zero today

/*
    Encodes I-type instructions in a 32-bit integer
    op: OpCode (7-bit)
    rd: Destination register (8-bit)
    kx | imm: Constant index (17 bit) or immediate value
*/
#define VM_ENCODE_I(op, rd, kx) ((((op) & 0x7F) << 25) | (((rd) & 0xFF) << 17) | ((kx) & 0x1FFFF))

#define VM_DECODE_I_RD(instr) (((instr) >> 17) & 0xFF) // Destination register
#define VM_DECODE_I_KX(instr) ((instr) & 0x1FFFF)      // 17-bit constant/index
#define VM_DECODE_I_IMM(instr) ((instr) & 0x1FFFF)     // 17-bit immediate value

#define VM_DECODE_OPCODE(instr) ((instr) >> 25) // Get OpCode (default to all types)

// Maximum constants supported by 17-bit index
#define VM_MAX_CONSTANTS ((1 << 17) - 1)

// Maximum registers supported with 8-bit
#define VM_MAX_REGISTERS ((1 << 8) - 1)

// OP_RETURN_N carries its slot count in the 8-bit r2 field.
#define VM_MAX_RETURN_SLOTS ((1 << 8) - 1)

// A pointer is a raw address, so it spans two slots and wants an even slot
// index to sit at its natural alignment.
#define VM_POINTER_SLOTS ((unsigned int)(sizeof(void *) / sizeof(Value)))

// Sentinel value for registers
#define VM_INVALID_REGISTER VM_MAX_REGISTERS + 1

#define value_list_item_free
GAB_LIST(ValueList, value_list, Value);

typedef struct {
    Chunk *chunk;
    int arity;
    int max_registers;
} FuncPrototype;

#define VM_MAX_CALL_DEPTH 256

typedef struct {
    const FuncPrototype *proto;

    size_t return_ip;

    // Byte offset into the stack, not a slot index.
    size_t base;
    unsigned int dest;
} CallFrame;

void func_proto_free(FuncPrototype proto);

#define func_proto_list_item_free(item) func_proto_free(item)
GAB_LIST(FuncProtoList, func_proto_list, FuncPrototype)

typedef struct {
    Arena *persistent_arena;
    Arena *transient_arena;

    StringPool strings; // must outlive global_scope
    Scope global_scope;

    ValueList global_data;
    FuncProtoList global_funcs;

    // The stack is byte-addressed and 8-byte aligned at the base, so a value
    // wider than a slot can sit at its natural alignment. Capacity is still
    // counted in slots; a slot is sizeof(Value).
    uint8_t *stack;
    size_t stack_capacity;

    // Points at stack + frame->base, in bytes.
    uint8_t *registers;

    CallFrame frames[VM_MAX_CALL_DEPTH];
    size_t frame_count;

    size_t instruction_pointer;
} VM;

// Register r of the current frame. A union member read gives well-defined type
// punning, which a cast from the raw byte pointer would not.
static inline Value *vm_reg(const VM *vm, size_t r) {
    return (Value *)(void *)(vm->registers + r * sizeof(Value));
}

// Slot i counted from the base of the stack, independent of any frame.
static inline Value *vm_slot(const VM *vm, size_t i) {
    return (Value *)(void *)(vm->stack + i * sizeof(Value));
}

VM *vm_create();
void vm_execute(VM *vm, const char *source);
void vm_free(VM *vm);

#endif
