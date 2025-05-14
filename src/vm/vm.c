#include "vm/vm.h"
#include "vm/constant_pool.h"
#include <stdlib.h>

#define CHUNK_INITIAL_CAPACITY 8

Chunk *chunk_create() {
    Chunk *chunk = malloc(sizeof(Chunk));
    chunk->instructions = malloc(sizeof(Instruction) * CHUNK_INITIAL_CAPACITY);
    chunk->instructions_size = 0;
    chunk->instructions_capacity = CHUNK_INITIAL_CAPACITY;
    chunk->const_pool = constpool_create(VM_MAX_CONSTANTS);
    return chunk;
}

void chunk_add_instruction(Chunk *chunk, Instruction instruction) {
    if (chunk->instructions_size >= chunk->instructions_capacity) {
        chunk->instructions_capacity *= 2;
        chunk->instructions =
            realloc(chunk->instructions, chunk->instructions_capacity * sizeof(Instruction));
    }

    chunk->instructions[chunk->instructions_size++] = instruction;
}

void chunk_free(Chunk *chunk) {
    constpool_free(chunk->const_pool);

    free(chunk->instructions);
    free(chunk);
}
