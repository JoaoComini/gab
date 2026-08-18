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
    op: OpCode (6-bit)
    rd: Destination register (7-bit)
    r1: Register 1 (7-bit)
    r2: Register 2 (7-bit)
*/
#define VM_ENCODE_R(op, rd, r1, r2) VM_ENCODE_R_FLAGS(op, rd, r1, r2, 0)

/*
    As VM_ENCODE_R, plus the 5 flag bits. Every field is masked: an
    out-of-range value would otherwise smear into its neighbours — including
    the opcode — and produce an instruction that matches no case.
*/
#define VM_ENCODE_R_FLAGS(op, rd, r1, r2, flags)                                                             \
    ((((op) & 0x3F) << 26) | (((rd) & 0x7F) << 19) | (((r1) & 0x7F) << 12) | (((r2) & 0x7F) << 5) |          \
     ((flags) & 0x1F))

#define VM_DECODE_R_RD(instr) (((instr) >> 19) & 0x7F) // Destination register
#define VM_DECODE_R_R1(instr) (((instr) >> 12) & 0x7F) // First source register
#define VM_DECODE_R_R2(instr) (((instr) >> 5) & 0x7F)  // Second source register
#define VM_DECODE_R_FLAGS(instr) ((instr) & 0x1F)      // Flags: field width, or return slot count

/*
    Encodes I-type instructions in a 32-bit integer
    op: OpCode (6-bit)
    rd: Destination register (7-bit)
    kx | imm: Constant index (19 bit) or immediate value
*/
#define VM_ENCODE_I(op, rd, kx) (((op) << 26) | ((rd) << 19) | ((kx) & 0x7FFFF))

#define VM_DECODE_I_RD(instr) (((instr) >> 19) & 0x7F) // Destination register
#define VM_DECODE_I_KX(instr) ((instr) & 0x7FFFF)      // 19-bit constant/index
#define VM_DECODE_I_IMM(instr) ((instr) & 0x7FFFF)     // 19-bit immediate value

#define VM_DECODE_OPCODE(instr) ((instr) >> 26) // Get OpCode (default to all types)

// Maximum constants supported by 19-bit index
#define VM_MAX_CONSTANTS ((1 << 19) - 1)

// Maximum registers supported with 7-bit
#define VM_MAX_REGISTERS ((1 << 7) - 1)

// OP_RETURN carries its slot count in the 5 flag bits.
#define VM_MAX_RETURN_SLOTS ((1 << 5) - 1)

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

    Value *stack;
    size_t stack_capacity;

    // Points at stack[frame->base], so a register access stays registers[r].
    Value *registers;

    CallFrame frames[VM_MAX_CALL_DEPTH];
    size_t frame_count;

    size_t instruction_pointer;
} VM;

VM *vm_create();
void vm_execute(VM *vm, const char *source);
void vm_free(VM *vm);

#endif
