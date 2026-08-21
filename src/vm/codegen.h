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

    // Read rather than appended to: the host bodies registered before this
    // compile, which an 'extern' declaration resolves against as its prototype
    // is created. Binding there rather than in a pass afterwards is what keeps
    // a prototype from ever existing unbound.
    const ExternBindingList *externs;
} CodegenOutput;

// Returns NULL if code generation failed; see the diagnostics sink.
// max_registers reports how many registers the top-level chunk addresses, so
// the caller can size its frame. Pass NULL if that is not needed.
// What the top-level chunk needs to run as a frame: its size, and the slots it
// may hold references in. Both are known only to codegen, and both travel with
// the chunk into CompiledScript.
typedef struct {
    unsigned int max_registers;
    FrameRefList refs;
} CodegenFrameInfo;

Chunk *codegen_generate(ASTScript *ast, CodegenOutput output, Diagnostics *diagnostics,
                        CodegenFrameInfo *frame_info);

#endif
