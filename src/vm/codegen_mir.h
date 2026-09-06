#ifndef GAB_CODEGEN_MIR_H
#define GAB_CODEGEN_MIR_H

#include "diagnostics.h"
#include "mir/mir.h"
#include "vm/chunk.h"
#include "vm/link.h"

#include <stdbool.h>

/* What only the unit can number: a callee's prototype and a boxed type's heap shape. A native callee
 * is numbered in its own table, so 'native' says which one the index belongs to. */
typedef struct {
    void *context;

    bool (*callee)(void *context, struct Function *callee, bool native, unsigned int *index, bool *relocates);

    void (*relocate_callee)(void *context, Chunk *chunk, size_t offset, bool native);

    /* Interning the shape a box allocates, and recording that its index needs relocating. */
    bool (*heap_shape)(void *context, const Type *type, unsigned int *index);

    void (*relocate_type)(void *context, Chunk *chunk, size_t offset);

    /* Interning the text a literal names, which the same relocation table follows. */
    bool (*string)(void *context, String *text, unsigned int *index);

    void (*relocate_string)(void *context, Chunk *chunk, size_t offset);
} MIRUnitNumbering;

/* Whether every instruction in a body is one the IR emitter handles, which decides the path it takes. */
bool codegen_mir_supports(const MIRFunction *ir);

typedef struct {
    Chunk *chunk;

    unsigned int max_registers;

    /* The slots an unwinding frame must release, since a trap reaches no drop the body emitted. */
    FrameRefList refs;

    /* The slot the body's first declared local was given, which a script's result is read from. */
    unsigned int first_local_slot;

    bool failed;
} MIREmission;

/* Emits a lowered body, reading the frame slot a register allocation gave each value. */
MIREmission codegen_mir_emit(Arena *arena, MIRFunction *ir, const MIRUnitNumbering *numbering,
                             Diagnostics *diagnostics);

#endif
