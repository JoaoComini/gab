#include "vm/vm.h"
#include <assert.h>
#include <stdio.h>

static void test_chunk_creation() {
    Chunk *chunk = chunk_create();

    // Verify initial state
    assert(chunk != NULL);
    assert(chunk->instructions != NULL);
    assert(chunk->instructions_size == 0);
    assert(chunk->instructions_capacity == VM_CHUNK_INITIAL_CAPACITY);
    assert(chunk->const_pool != NULL);

    chunk_free(chunk);
}

static void test_instruction_addition() {
    Chunk *chunk = chunk_create();
    const Instruction instructions[] = {0xDEADBEEF, 0xCAFEBABE, 0x12345678};

    for (size_t i = 0; i < 3; i++) {
        chunk_add_instruction(chunk, instructions[i]);
        assert(chunk->instructions_size == i + 1);
        assert(chunk->instructions[i] == instructions[i]);
    }

    assert(chunk->instructions_capacity == VM_CHUNK_INITIAL_CAPACITY);

    chunk_free(chunk);
}

static void test_chunk_resize() {
    Chunk *chunk = chunk_create();

    for (size_t i = 0; i < VM_CHUNK_INITIAL_CAPACITY; i++) {
        chunk_add_instruction(chunk, i);
    }

    assert(chunk->instructions_capacity == VM_CHUNK_INITIAL_CAPACITY);

    // Trigger resize
    chunk_add_instruction(chunk, 0xFFFF);
    assert(chunk->instructions_capacity == VM_CHUNK_INITIAL_CAPACITY * 2);
    assert(chunk->instructions_size == VM_CHUNK_INITIAL_CAPACITY + 1);

    chunk_free(chunk);
}

static void test_vm_execute() {
    VM *vm = vm_create();

    vm_execute(vm, "let x = 2; return (x + 3) * ((15 / 5) - 1);");

    vm_free(vm);
}

int main() {
    test_chunk_creation();
    test_instruction_addition();
    test_chunk_resize();
    test_vm_execute();

    return 0;
}
