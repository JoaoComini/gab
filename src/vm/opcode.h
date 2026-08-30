#ifndef GAB_OPCODE_H
#define GAB_OPCODE_H

#include "object.h"
#include "slot.h"

#include <stdint.h>

typedef enum {
    OP_LOAD_CONST,
    OP_LOAD_TRUE,
    OP_LOAD_FALSE,

    OP_LOAD_STR,
    OP_MOVE,

    OP_MOVE_N,
    OP_ADDI,
    OP_SUBI,
    OP_MULI,
    OP_DIVI,

    OP_MODI,

    OP_NEGI,
    OP_NEGF,

    OP_ITOF,
    OP_FTOI,
    OP_CMP_LTI,
    OP_CMP_GTI,
    OP_CMP_EQI,
    OP_CMP_NEI,
    OP_CMP_LEI,
    OP_CMP_GEI,
    OP_ADDF,
    OP_SUBF,
    OP_MULF,
    OP_DIVF,

    OP_ADDFK,
    OP_SUBFK,
    OP_MULFK,
    OP_DIVFK,
    OP_CMP_LTF,
    OP_CMP_GTF,

    OP_CMP_EQS,
    OP_CMP_NES,

    OP_CMP_EQF,
    OP_CMP_NEF,
    OP_CMP_LEF,
    OP_CMP_GEF,

    OP_CMP_LTFK,
    OP_CMP_GTFK,
    OP_CMP_LEFK,
    OP_CMP_GEFK,
    OP_CMP_EQFK,
    OP_CMP_NEFK,
    OP_JMP,
    OP_JMP_IF_FALSE,
    OP_JMP_IF_TRUE,
    OP_CALL,

    OP_CALL_EXTERN,

    OP_NEW,

    OP_NULL,

    OP_RELEASE,

    OP_RETURN,
    OP_RETURN_N,

    OP_LOAD_FIELD_1,
    OP_LOAD_FIELD_2,
    OP_LOAD_FIELD_4,
    OP_STORE_FIELD_1,
    OP_STORE_FIELD_2,
    OP_STORE_FIELD_4,

    OP_ADDR_OF,

    OP_LOAD_FIELD_PTR_1,
    OP_LOAD_FIELD_PTR_2,
    OP_LOAD_FIELD_PTR_4,
    OP_STORE_FIELD_PTR_1,
    OP_STORE_FIELD_PTR_2,
    OP_STORE_FIELD_PTR_4,

    OP_ADD_PTR,

    OP_ALLOC,

    OP_FREE,

    OP_ADD_PTR_REG,

    OP_BOUNDS_CHECK,

    OP_LOAD_PTR_N,
    OP_STORE_PTR_N,

    OP_LOOP_INIT,

    OP_FOR_LOOP,

    OP__COUNT,
} OpCode;

#define VM_ENCODE_R(op, rd, r1, r2) VM_ENCODE_RK(op, rd, r1, r2, 0)

#define VM_ENCODE_RK(op, rd, r1, r2, k)                                                                      \
    ((((uint32_t)(op) & 0x7Fu) << 25) | (((uint32_t)(rd) & 0xFFu) << 17) | (((uint32_t)(r1) & 0xFFu) << 9) | \
     (((uint32_t)(r2) & 0xFFu) << 1) | ((uint32_t)(k) & 0x1u))

#define VM_DECODE_R_RD(instr) (((instr) >> 17) & 0xFF)
#define VM_DECODE_R_R1(instr) (((instr) >> 9) & 0xFF)
#define VM_DECODE_R_R2(instr) (((instr) >> 1) & 0xFF)
#define VM_DECODE_R_K(instr) ((instr) & 0x1)

#define VM_MAX_IMMEDIATE 0xFF

#define VM_DECODE_R_BACK(instr) (((instr) >> 1) & 0xFF)

#define VM_MAX_LOOP_OFFSET 127

#define VM_ENCODE_I(op, rd, kx)                                                                              \
    ((((uint32_t)(op) & 0x7Fu) << 25) | (((uint32_t)(rd) & 0xFFu) << 17) | ((uint32_t)(kx) & 0x1FFFFu))

#define VM_DECODE_I_RD(instr) (((instr) >> 17) & 0xFF)
#define VM_DECODE_I_KX(instr) ((instr) & 0x1FFFF)
#define VM_DECODE_I_IMM(instr) ((instr) & 0x1FFFF)

#define VM_DECODE_I_SIMM(instr) ((int32_t)((uint32_t)(instr) << 15) >> 15)

#define VM_DECODE_OPCODE(instr) ((instr) >> 25)

#define VM_MAX_CONSTANTS ((1 << 17) - 1)

#define VM_MAX_JUMP ((1 << 16) - 1)

#define VM_MAX_REGISTERS ((1 << 8) - 1)

#define VM_MAX_PROTOTYPES VM_MAX_CONSTANTS

#define VM_MAX_EXTERN_PROTOS VM_MAX_CONSTANTS

#define VM_MAX_HEAP_TYPES VM_MAX_CONSTANTS
#define VM_MAX_STRINGS VM_MAX_CONSTANTS

#define VM_STRING_SLOTS ((unsigned int)(sizeof(GabStringValue) / VM_SLOT_SIZE))

#define VM_MAX_FRAME_SLOTS ((1 << 8) - 1)

#define VM_MAX_FIELD_OFFSET ((1 << 8) - 1)

#define VM_MAX_STRUCT_SLOTS ((1 << 8) - 1)

#define VM_MAX_RETURN_SLOTS ((1 << 8) - 1)

#define VM_MAX_MOVE_SLOTS ((1 << 8) - 1)

#define VM_INDIRECT_SLOTS ((unsigned int)(sizeof(void *) / VM_SLOT_SIZE))

#define VM_INVALID_REGISTER VM_MAX_REGISTERS + 1

#endif
