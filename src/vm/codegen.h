#ifndef GAB_CODEGEN_H
#define GAB_CODEGEN_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "vm/chunk.h"
#include "vm/vm.h"

// The VM-wide tables a compile appends to, rather than produces: a prototype
// index and a type index are both baked into instructions, so both outlive the
// compile that made them and belong to the VM. Bundled because they are always
// passed together and a third would otherwise keep widening the signature.
typedef struct {
    FuncProtoList *funcs;
    TypeList *heap_types;
} CodegenOutput;

// Returns NULL if code generation failed; see the diagnostics sink.
// max_registers reports how many registers the top-level chunk addresses, so
// the caller can size its frame. Pass NULL if that is not needed.
Chunk *codegen_generate(ASTScript *ast, CodegenOutput output, Diagnostics *diagnostics,
                        unsigned int *max_registers);

#endif
