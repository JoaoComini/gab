#include "chunk.h"

#include "vm/vm.h"

Chunk *chunk_create() {
    Chunk *chunk = malloc(sizeof(Chunk));
    chunk->instructions = instruction_list_create();
    chunk->const_pool = constpool_create(VM_MAX_CONSTANTS);
    return chunk;
}

size_t chunk_add_instruction(Chunk *chunk, Instruction instruction) {
    instruction_list_add(&chunk->instructions, instruction);

    return chunk->instructions.size - 1;
}

void chunk_patch_instruction(Chunk *chunk, size_t index, Instruction instruction) {
    instruction_list_emplace(&chunk->instructions, index, instruction);
}

void chunk_free(Chunk *chunk) {
    constpool_free(chunk->const_pool);
    instruction_list_free(&chunk->instructions);

    free(chunk);
}
