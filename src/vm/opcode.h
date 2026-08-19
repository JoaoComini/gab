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

    // Allocates a heap object of heap_types[kx] into rd, with one strong
    // reference. I-type: a type index is not a register.
    OP_NEW,

    // Drops one strong reference from the pointer in rd, freeing at zero. The
    // slot keeps whatever it held: nothing reads it again, since codegen only
    // emits this where the value goes out of scope.
    OP_RELEASE,

    // Adds one strong reference to the pointer in rd. Emitted where a borrowed
    // reference is stored somewhere that outlives the statement, since the slot
    // it came from still owns its own.
    OP_RETAIN,

    // The weak counterparts. A weak reference does not keep its object alive,
    // so these touch the weak count and never the strong one.
    OP_RETAIN_WEAK,
    OP_RELEASE_WEAK,

    // Fails the run if the weak pointer in rd names an object whose payload is
    // gone. Emitted only where a weak reference is reached through, which is
    // the one place the answer can change a program.
    OP_CHECK_ALIVE,

    // Writes a null pointer into the two slots at rd. A weak local owns its slot
    // from the moment it is declared, so the slot has to start as something
    // every weak operation tolerates rather than as whatever the stack held.
    OP_LOAD_NULL_PTR,
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
} OpCode;

#endif
