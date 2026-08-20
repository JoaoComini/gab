#ifndef GAB_OPCODE_H
#define GAB_OPCODE_H

typedef enum {
    OP_LOAD_CONST,
    OP_LOAD_TRUE,
    OP_LOAD_FALSE,
    OP_MOVE,

    // Copies a run of slots between frame slots, the register-to-register
    // counterpart of OP_LOAD_PTR_N. A struct or a pointer is several slots, and
    // one instruction per slot spends a dispatch on each; the count is a
    // compile-time constant, so it rides in the spare third operand for free.
    //
    // OP_MOVE stays for the single-slot case, which is every scalar: decoding a
    // third operand and calling memmove to move four bytes would be slower than
    // the assignment it replaced.
    OP_MOVE_N,
    OP_ADDI,
    OP_SUBI,
    OP_MULI,
    OP_DIVI,

    // Remainder. Int-only: there is no OP_MODF, because a float remainder is
    // fmodf rather than a hardware instruction. Shares OP_DIVI's two undefined
    // operand pairs, since it is computed by the same instruction.
    OP_MODI,

    // Numeric conversion. Neither can fail: OP_FTOI clamps a float that does not
    // fit to the nearest end of the int range, so every operand has an answer.
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

    // Allocates a heap object of heap_types[kx] into rd. I-type: a type index
    // is not a register.
    OP_NEW,

    // Frees the object in rd, and everything it owns. The slot keeps whatever
    // it held: nothing reads it again, since codegen only emits this where the
    // value goes out of scope.
    //
    // There is no counterpart. Ownership is unique and static — exactly one
    // slot owns an object — so nothing ever needs to claim a second share of
    // one, and a 'ref T' claims none.
    OP_RELEASE,

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
    OP_STORE_FIELD_4,

    // Writes the address of a frame slot into a 2-slot destination.
    OP_ADDR_OF,

    // As the OP_LOAD_FIELD_* / OP_STORE_FIELD_* family, except the base names
    // a slot pair holding an address rather than the struct itself.
    OP_LOAD_FIELD_PTR_1,
    OP_LOAD_FIELD_PTR_2,
    OP_LOAD_FIELD_PTR_4,
    OP_STORE_FIELD_PTR_1,
    OP_STORE_FIELD_PTR_2,
    OP_STORE_FIELD_PTR_4,

    // Adds a byte offset to an address, for reaching a field through a pointer.
    OP_ADD_PTR,

    // Copies a run of slots to or from the address a slot pair holds. The slot
    // count rides in the third operand, so a whole struct moves in one step.
    OP_LOAD_PTR_N,
    OP_STORE_PTR_N,

    // One iteration of a counting loop: step rd, and if it is still below r1,
    // jump back by the signed offset in r2. Replaces the compare, the
    // conditional jump, the increment and the jump back that a general loop
    // needs -- four dispatches per iteration rather than one.
    //
    // The step is fixed at one and the comparison at '<', because that is the
    // shape codegen recognises; anything else keeps the general form.
    OP_FOR_LOOP,

    // Not an instruction: the number of them. The dispatch table in vm.c is
    // sized by this and must have an entry for every opcode below it, so a new
    // opcode added without one fails to build rather than jumping nowhere.
    OP__COUNT,
} OpCode;

#endif
