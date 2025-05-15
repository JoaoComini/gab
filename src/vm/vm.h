#ifndef RULE_VM_H
#define RULE_VM_H

#include "variant.h"
#include "vm/constant_pool.h"
#include <stdint.h>

typedef uint32_t Instruction;

/*
    Encodes R-type instructions in a 32-bit integer
    op: OpCode (6-bit)
    rd: Destination register (7-bit)
    r1: Register 1 (7-bit)
    r2: Register 2 (7-bit)
*/
#define VM_ENCODE_R(op, rd, r1, r2) (((op) << 26) | ((rd) << 19) | ((r1) << 12) | ((r2) << 5))

#define VM_DECODE_R_RD(instr) (((instr) >> 19) & 0x7F) // Destination register
#define VM_DECODE_R_R1(instr) (((instr) >> 12) & 0x7F) // First source register
#define VM_DECODE_R_R2(instr) (((instr) >> 5) & 0x7F)  // Second source register
#define VM_DECODE_R_FLAGS(instr) ((instr) & 0x1F)      // Flags (unused)

/*
    Encodes I-type instructions in a 32-bit integer
    op: OpCode (6-bit)
    rd: Destination register (7-bit)
    kx: Constant index (19 bit)
*/
#define VM_ENCODE_I(op, rd, kx) (((op) << 26) | ((rd) << 19) | ((kx) & 0x7FFFF))

#define VM_DECODE_I_RD(instr) (((instr) >> 19) & 0x7F) // Destination register
#define VM_DECODE_I_KX(instr) ((instr) & 0x7FFFF)      // 19-bit constant/index

#define VM_DECODE_OPCODE(instr) ((instr) >> 26) // Get OpCode (default to all types)

// Maximum constants supported by 19-bit index
#define VM_MAX_CONSTANTS ((1 << 19) - 1)

// Maximum registers supported with 7-bit
#define VM_MAX_REGISTERS ((1 << 7) - 1)

// Initial number of the instructions array in a Chunk
#define VM_CHUNK_INITIAL_CAPACITY 4

typedef struct {
    ConstantPool *const_pool;
    Instruction *instructions;
    size_t instructions_capacity;
    size_t instructions_size;
} Chunk;

Chunk *chunk_create();
void chunk_add_instruction(Chunk *chunk, Instruction instruction);
void chunk_free(Chunk *chunk);

typedef struct {
    Variant registers[VM_MAX_REGISTERS];

    size_t instruction_pointer;
} VM;

VM *vm_create();
void vm_execute(VM *vm, const char *source);
void vm_free(VM *vm);

#endif
