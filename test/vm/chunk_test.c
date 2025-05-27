#include "vm/chunk.h"

#include <assert.h>
#include <stdio.h>

static void test_chunk_creation() {
    Chunk *chunk = chunk_create();

    // Verify initial state
    assert(chunk != NULL);
    assert(chunk->instructions.size == 0);
    assert(chunk->const_pool != NULL);

    chunk_free(chunk);
}

static void test_instruction_addition() {
    Chunk *chunk = chunk_create();
    const Instruction instructions[] = {0xDEADBEEF, 0xCAFEBABE, 0x12345678};

    for (size_t i = 0; i < 3; i++) {
        size_t index = chunk_add_instruction(chunk, instructions[i]);
        assert(chunk->instructions.size == i + 1);
        assert(chunk->instructions.data[i] == instructions[i]);
        assert(index == i);
    }

    chunk_free(chunk);
}

int main() {
    test_chunk_creation();
    test_instruction_addition();

    return 0;
}
