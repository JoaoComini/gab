#ifndef GAB_OPCODE_H
#define GAB_OPCODE_H

typedef enum {
    OP_LOAD_CONST, // Rdest ← Constant (I-type)
    OP_MOVE,       // Rdest ← Rsrc1 (R-type)
    OP_ADD,        // Rdest ← Rsrc1 + Rsrc2 (R-type)
    OP_SUB,        // Rdest ← Rsrc1 - Rsrc2 (R-type)
    OP_MUL,        // Rdest ← Rsrc1 * Rsrc2 (R-type)
    OP_DIV,        // Rdest ← Rsrc1 / Rsrc2 (R-type)
    OP_CMP_LT,
    OP_CMP_GT,
    OP_CMP_EQ,
    OP_CMP_NE,
    OP_CMP_LE,
    OP_CMP_GE,
    OP_JMP,
    OP_JMP_IF_FALSE,
    OP_RETURN // Exit with R0 (no operands)
} OpCode;

#endif
