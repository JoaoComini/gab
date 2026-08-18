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
    OP_RETURN,
    OP_LOAD_FIELD,
    OP_STORE_FIELD
} OpCode;

// Field access is byte-granular: sub-word fields share a slot, so a slot-wide
// store would clobber a field's neighbours. Width travels in the R-type flag
// bits as this enum rather than as a raw byte count.
typedef enum {
    FIELD_WIDTH_1,
    FIELD_WIDTH_2,
    FIELD_WIDTH_4,
} FieldWidth;

#endif
