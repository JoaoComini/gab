#ifndef GAB_OPCODE_H
#define GAB_OPCODE_H

typedef enum {
    OP_LOAD_CONST,
    OP_LOAD_GLOBAL,
    OP_STORE_GLOBAL,
    OP_LOAD_TRUE,
    OP_LOAD_FALSE,
    OP_MOVE,
    OP_ADDI,
    OP_SUBI,
    OP_MULI,
    OP_DIVI,
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
    OP_CMP_LTF,
    OP_CMP_GTF,
    OP_CMP_EQF,
    OP_CMP_NEF,
    OP_CMP_LEF,
    OP_CMP_GEF,
    OP_JMP,
    OP_JMP_IF_FALSE,
    OP_JMP_IF_TRUE,
    OP_CALL,
    // OP_RETURN returns a single slot, the common case; OP_RETURN_N carries a
    // slot count in r2.
    OP_RETURN,
    OP_RETURN_N,

    // Field access is byte-granular: sub-word fields share a slot, so a
    // slot-wide store would clobber a field's neighbours. The width is a
    // compile-time constant, so it selects the opcode rather than costing
    // operand bits.
    OP_LOAD_FIELD_1,
    OP_LOAD_FIELD_2,
    OP_LOAD_FIELD_4,
    OP_STORE_FIELD_1,
    OP_STORE_FIELD_2,
    OP_STORE_FIELD_4
} OpCode;

#endif
