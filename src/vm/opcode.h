#ifndef GAB_OPCODE_H
#define GAB_OPCODE_H

#include "object.h"
#include "slot.h"

#include <stdint.h>

typedef enum {
    OP_LOAD_CONST,
    OP_LOAD_TRUE,
    OP_LOAD_FALSE,

    // Writes a literal's header -- the address of its characters and their
    // count -- into the slots at rd. I-type: a string index is not a register.
    //
    // The characters are interned in the unit's arena and outlive every frame,
    // so the header borrows them and nothing frees them.
    OP_LOAD_STR,
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

    // Negation, rd = -r1. A unary form rather than a subtraction from zero,
    // which would spend a register and a load on the zero -- and does so on
    // every execution, where a literal operand folds at compile time and never
    // reaches here at all.
    //
    // OP_NEGI wraps INT32_MIN rather than overflowing, computing on the
    // unsigned width for the same reason the folder does.
    OP_NEGI,
    OP_NEGF,

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

    // As the four above, with the right operand read from the constant pool by
    // the index in r2 rather than from a register. A float literal has no
    // eight-bit encoding, so without these every 'x + 1.5' costs a load of its
    // own -- which is most of what float arithmetic is made of.
    //
    // The index is 8 bits, so a chunk past its 256th constant falls back to the
    // register form. Constants are pooled per function and deduplicated, so
    // that bound is far past any function anyone writes.
    OP_ADDFK,
    OP_SUBFK,
    OP_MULFK,
    OP_DIVFK,
    OP_CMP_LTF,
    OP_CMP_GTF,

    // String equality: rd becomes whether the strings in r1 and r2 spell the
    // same characters. Not a slot comparison -- two headers may name different
    // addresses and still be equal -- so it reads the lengths and then the
    // characters.
    //
    // Interning makes equal literals one address, which the comparison takes as
    // its fast path rather than as its definition: a string built at runtime
    // would never be interned, so identity alone would answer wrongly.
    OP_CMP_EQS,
    OP_CMP_NES,

    // Joins two strings into a freshly allocated one, whose characters are the
    // payload of a heap object the destination slot then owns. Allocating is
    // what makes the result owning: neither operand's characters can grow in
    // place, and the arena's belong to the unit rather than to a frame.
    OP_CONCAT,
    OP_CMP_EQF,
    OP_CMP_NEF,
    OP_CMP_LEF,
    OP_CMP_GEF,

    // The float comparisons with the right operand read from the constant pool,
    // as the arithmetic K forms do. 'p.y < 0.0' is the shape that wants them:
    // comparing against a literal is most of what a float comparison is, and
    // without these each one costs a load of its own.
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

    // Calls extern_protos[kx], whose body is C. I-type like OP_CALL, but into
    // the other table: the two are numbered separately, so the same kx names a
    // different function in each.
    //
    // No frame is pushed. A C body has no bytecode to interpret and no
    // instruction pointer to return to, and its arguments are already laid out
    // where a callee's would be, so it runs in the caller's frame.
    OP_CALL_EXTERN,

    // Allocates a heap object of heap_types[kx] into rd. I-type: a type index
    // is not a register.
    OP_NEW,

    // Writes a null pointer into the slots at rd. I-type: rd is the only
    // operand, and the value is the same every time.
    //
    // For a slot that must be safe to free before anything has been stored in
    // it: an owning field of a struct local holds whatever the frame last left
    // there, and the first store into it frees what it finds. Writing the
    // pointer as a pointer rather than as zeroed slots keeps the width right
    // wherever a pointer is not two slots wide.
    OP_NULL,

    // Frees what the value at rd owns, by the drop of heap_types[kx]. I-type
    // for the type index, which is relocated at link like OP_NEW's.
    //
    // Typed rather than reading the object's own header, because what a slot
    // holds is not always a pointer: an array is a header whose length the free
    // needs, and that is beside the pointer rather than in the block. Knowing
    // the type here is also what lets the drop be the one chosen when the
    // layout was computed, rather than rediscovered per free.
    //
    // The slot keeps whatever it held: nothing reads it again, since codegen
    // only emits this where the value goes out of scope.
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

    // Allocates the block for the array header at rd, whose type is
    // heap_types[kx]. The count is read from the header's own length slot,
    // which codegen has already written, and the address is written beside it.
    // Fails the run on a negative count or an allocation that does not succeed.
    //
    // I-type, so the count rides in the header rather than in an operand: a
    // type index is relocated at link and only this format is patched.
    //
    // Takes a type index rather than a width because the block is typed by the
    // array that owns it -- freeing it walks the elements, and only the type
    // says how.
    OP_ARRAY_NEW,

    // As OP_ADD_PTR, with the offset read from a register rather than an
    // operand. An element's offset is the index times the stride, which is not
    // known until the index is.
    OP_ADD_PTR_REG,

    // Traps unless 0 <= r1 < the length in the header at rd. Separate from the
    // access that follows so that the access stays the same instruction a field
    // uses -- a bounds check is about the index, not about the load.
    OP_BOUNDS_CHECK,

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

/*
    Encodes R-type instructions in a 32-bit integer
    op: OpCode (7-bit)
    rd: Destination register (8-bit)
    r1: Register 1 (8-bit)
    r2: Register 2 (8-bit)
    k:  when set, r2 is a small unsigned immediate rather than a register

    The k bit is Lua's register-or-constant operand trick: 'x + 1' would
    otherwise need a LOAD_CONST into a register the arithmetic then reads once
    and never again, and small literals are most of what arithmetic operates on.
    Only the second operand can be immediate, which is enough because the
    commutative ops are emitted with the constant on the right.
*/
#define VM_ENCODE_R(op, rd, r1, r2) VM_ENCODE_RK(op, rd, r1, r2, 0)

/*
    As VM_ENCODE_R, plus the spare k bit. Every field is masked: an
    out-of-range value would otherwise smear into its neighbours — including
    the opcode — and produce an instruction that matches no case.

    The masks are unsigned because the opcode field reaches bit 31: an int
    shifted that far is undefined once the opcode passes 63, which is a limit
    the table would otherwise hit silently as it grows.
*/
#define VM_ENCODE_RK(op, rd, r1, r2, k)                                                                      \
    ((((uint32_t)(op) & 0x7Fu) << 25) | (((uint32_t)(rd) & 0xFFu) << 17) | (((uint32_t)(r1) & 0xFFu) << 9) | \
     (((uint32_t)(r2) & 0xFFu) << 1) | ((uint32_t)(k) & 0x1u))

#define VM_DECODE_R_RD(instr) (((instr) >> 17) & 0xFF) // Destination register
#define VM_DECODE_R_R1(instr) (((instr) >> 9) & 0xFF)  // First source register
#define VM_DECODE_R_R2(instr) (((instr) >> 1) & 0xFF)  // Second source register
#define VM_DECODE_R_K(instr) ((instr) & 0x1)           // r2 is an immediate, not a register

// The widest immediate the r2 field holds. A literal above this is loaded into
// a register as before, so the range is a codegen decision and never a limit on
// what a program can say.
#define VM_MAX_IMMEDIATE 0xFF

// The r2 field read as a signed jump offset, for OP_FOR_LOOP. Sign-extended by
// the shift pair rather than by a cast, since the field is not a whole type's
// width -- the same reason VM_DECODE_I_SIMM is written this way.
#define VM_DECODE_R_SIMM(instr) ((int32_t)((uint32_t)(instr) << 23) >> 24)

// How far the fused loop reaches back. A body longer than this keeps the
// general compare-and-jump form, so the range bounds an optimisation rather
// than a program.
#define VM_MAX_LOOP_OFFSET 127

/*
    Encodes I-type instructions in a 32-bit integer
    op: OpCode (7-bit)
    rd: Destination register (8-bit)
    kx | imm: Constant index (17 bit) or immediate value

    The form for an instruction naming one register plus something that is not
    a register, where 8 bits would be too narrow: a constant index, or a
    prototype index. R-type's three 8-bit fields suit an instruction whose
    operands are all register indices; this suits the rest.
*/
#define VM_ENCODE_I(op, rd, kx)                                                                              \
    ((((uint32_t)(op) & 0x7Fu) << 25) | (((uint32_t)(rd) & 0xFFu) << 17) | ((uint32_t)(kx) & 0x1FFFFu))

#define VM_DECODE_I_RD(instr) (((instr) >> 17) & 0xFF) // Destination register
#define VM_DECODE_I_KX(instr) ((instr) & 0x1FFFF)      // 17-bit constant/index
#define VM_DECODE_I_IMM(instr) ((instr) & 0x1FFFF)     // 17-bit immediate value

// The same 17 bits read as a signed jump offset. A jump is the one I-type
// operand with a direction: an index into the constant or prototype table
// counts from zero and never backwards, while a jump target may lie either side
// of the jump itself. Sign-extended by the shift pair rather than by a cast,
// since the field is not a whole type's width.
#define VM_DECODE_I_SIMM(instr) ((int32_t)((uint32_t)(instr) << 15) >> 15)

#define VM_DECODE_OPCODE(instr) ((instr) >> 25) // Get OpCode (default to all types)

// Maximum constants supported by 17-bit index
#define VM_MAX_CONSTANTS ((1 << 17) - 1)

// How far one jump reaches. The offset spends a bit on its sign, so it spans
// half what an index of the same width does, in either direction.
#define VM_MAX_JUMP ((1 << 16) - 1)

// Maximum registers supported with 8-bit
#define VM_MAX_REGISTERS ((1 << 8) - 1)

/*
    The limits below are named for what they bound rather than sharing one
    constant, because they are different quantities that mostly coincide at
    255 — the width of an 8-bit operand field. Widening one instruction's
    field must not silently move the others.

    VM_MAX_PROTOTYPES is why this matters. It was 255 while a prototype index
    rode in OP_CALL's 8-bit register field, which capped a whole VM at 255
    functions across every module it loaded — far too few for a real project,
    and an arbitrary limit besides, since a prototype index is not a register
    and only sat in a register-sized field by accident.
*/

// A prototype index rides in OP_CALL's 17-bit I-type field, so it is bounded
// like a constant index rather than like a register.
#define VM_MAX_PROTOTYPES VM_MAX_CONSTANTS

// An extern index rides in OP_CALL_EXTERN's 17-bit I-type field, and is its
// own space: a program may hold this many extern bodies and that many
// prototypes, neither counting against the other.
#define VM_MAX_EXTERN_PROTOS VM_MAX_CONSTANTS

// A type index rides in OP_NEW's 17-bit I-type field, for the same reason.
#define VM_MAX_HEAP_TYPES VM_MAX_CONSTANTS
#define VM_MAX_STRINGS VM_MAX_CONSTANTS

// A string value is an address and a count. The count is padded out to the
// address's alignment, which is what makes the script's layout the C one -- so it
// costs a slot more than its two fields need.
#define VM_STRING_SLOTS ((unsigned int)(sizeof(GabStringValue) / VM_SLOT_SIZE))

// The slots one frame addresses, which is what a register operand indexes.
#define VM_MAX_FRAME_SLOTS ((1 << 8) - 1)

// A field's byte offset within a struct rides in an 8-bit operand.
#define VM_MAX_FIELD_OFFSET ((1 << 8) - 1)

// A struct's width in slots, carried in an 8-bit operand by the opcodes that
// move a whole struct at once.
#define VM_MAX_STRUCT_SLOTS ((1 << 8) - 1)

// OP_RETURN_N carries its slot count in the 8-bit r2 field.
#define VM_MAX_RETURN_SLOTS ((1 << 8) - 1)

// Widest run OP_MOVE_N can carry, bounded by its 8-bit count field.
#define VM_MAX_MOVE_SLOTS ((1 << 8) - 1)

// A pointer is a raw address, so it spans two slots and wants an even slot
// index to sit at its natural alignment.
#define VM_INDIRECT_SLOTS ((unsigned int)(sizeof(void *) / VM_SLOT_SIZE))

// Sentinel value for registers
#define VM_INVALID_REGISTER VM_MAX_REGISTERS + 1

#endif
