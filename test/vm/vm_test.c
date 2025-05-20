#include "variant.h"
#include "vm/vm.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void test_chunk_creation() {
    Chunk *chunk = chunk_create();

    // Verify initial state
    assert(chunk != NULL);
    assert(chunk->instructions != NULL);
    assert(chunk->size == 0);
    assert(chunk->capacity == VM_CHUNK_INITIAL_CAPACITY);
    assert(chunk->const_pool != NULL);

    chunk_free(chunk);
}

static void test_instruction_addition() {
    Chunk *chunk = chunk_create();
    const Instruction instructions[] = {0xDEADBEEF, 0xCAFEBABE, 0x12345678};

    for (size_t i = 0; i < 3; i++) {
        chunk_add_instruction(chunk, instructions[i]);
        assert(chunk->size == i + 1);
        assert(chunk->instructions[i] == instructions[i]);
    }

    assert(chunk->capacity == VM_CHUNK_INITIAL_CAPACITY);

    chunk_free(chunk);
}

static void test_chunk_resize() {
    Chunk *chunk = chunk_create();

    for (size_t i = 0; i < VM_CHUNK_INITIAL_CAPACITY; i++) {
        chunk_add_instruction(chunk, i);
    }

    assert(chunk->capacity == VM_CHUNK_INITIAL_CAPACITY);

    // Trigger resize
    chunk_add_instruction(chunk, 0xFFFF);
    assert(chunk->capacity == VM_CHUNK_INITIAL_CAPACITY * 2);
    assert(chunk->size == VM_CHUNK_INITIAL_CAPACITY + 1);

    chunk_free(chunk);
}

static void test_vm_execute() {
    VM *vm = vm_create();

    vm_execute(vm, "let a = 2;\n"
                   "let b = 3;\n"
                   "let c = a + b * 5;\n"
                   "let d = (c - a) * ((b + 4) / (a + 1));\n"
                   "let e = d + (c * (a - b) + (b / (a + 2)));\n"
                   "let f = e - ((d + c) * (b - a));\n"
                   "let g = ((f + e) * (d - c)) / ((a + b) - (e / (d + 1)));\n"
                   "let h = g + f - e * (d + c - (b * a));\n"
                   "let i = (h / g) + (f - (e * (d / (c + (b - a)))));\n"
                   "let result = ((i + h) * (g - f) + (e / d)) - ((c + b) * (a - 1));\n"
                   "if result > 20000 { return 1; } else { return 0; }");

    Variant result = vm_get_result(vm);

    assert(result.type == VARIANT_NUMBER);
    assert(result.number_var == 1);

    vm_free(vm);
}

int main() {
    test_chunk_creation();
    test_instruction_addition();
    test_chunk_resize();
    test_vm_execute();

    return 0;
}
