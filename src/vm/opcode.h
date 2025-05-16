#ifndef GAB_OPCODE_H
#define GAB_OPCODE_H

typedef enum {
    OP_LOAD_CONST, // Rdest ← Constant (I-type)
    OP_ADD,        // Rdest ← Rsrc1 + Rsrc2 (R-type)
    OP_SUB,        // Rdest ← Rsrc1 - Rsrc2 (R-type)
    OP_MUL,        // Rdest ← Rsrc1 * Rsrc2 (R-type)
    OP_DIV,        // Rdest ← Rsrc1 / Rsrc2 (R-type)
    OP_RETURN      // Exit with R0 (no operands)
} OpCode;

#endif
