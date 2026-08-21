#include "compile.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Heap pointers need 8-byte alignment. A 4-byte-aligned base
// would put half the even slots on a 4-byte boundary, so the guarantee is on
// the base of the stack rather than on any particular slot.
static bool is_8_byte_aligned(const void *p) { return ((uintptr_t)p & 7u) == 0; }

static void test_stack_base_is_aligned_at_creation() {
    VM *vm = vm_create();

    assert(is_8_byte_aligned(vm->stack));
    assert(vm->stack == vm->registers);

    vm_free(vm);
}

// The stack is reserved once and never reallocated, because an address into it
// must stay valid for as long as what it points at. A recursion deep enough to
// have forced a reallocation must leave the buffer exactly where it was.
static void test_stack_does_not_move_under_deep_recursion() {
    VM *vm = vm_create();

    const uint8_t *before = vm->stack;
    size_t capacity_before = vm->stack_capacity;

    compile_and_run(vm, "module test;\n"
                        "func down(n: int): int {\n"
                        "if n <= 0 { return 0; }\n"
                        "let a = n + 1; let b = n + 2; let c = n + 3;\n"
                        "return down(n - 1) + a + b + c;\n"
                        "}\n"
                        "let r: int = down(200);\n");

    assert(vm->stack == before);
    assert(vm->stack_capacity == capacity_before);

    assert(is_8_byte_aligned(vm->stack));

    vm_free(vm);
}

// Deep recursion must not disturb the frames already on the stack: a value
// written before the recursion has to still be readable after it.
static void test_deep_recursion_preserves_live_frames() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "func down(n: int): int {\n"
                        "if n <= 0 { return 0; }\n"
                        "let keep = n;\n"
                        "let rest = down(n - 1);\n"
                        "return keep + rest;\n"
                        "}\n"
                        "let r: int = down(200);\n");

    // 200 + 199 + ... + 1: every frame's 'keep' is still there.
    int32_t result;
    memcpy(&result, vm_slot_at(vm, 0), sizeof(result));

    assert(result == 20100);

    vm_free(vm);
}

// A register is a slot index scaled by VM_SLOT_SIZE, so consecutive registers
// are exactly one slot apart in bytes.
static void test_registers_are_slot_granular() {
    VM *vm = vm_create();

    assert(vm_reg_at(vm, 1) - vm_reg_at(vm, 0) == VM_SLOT_SIZE);
    assert(vm_slot_at(vm, 4) - vm->stack == 4 * VM_SLOT_SIZE);

    vm_free(vm);
}

int main() {
    test_stack_base_is_aligned_at_creation();
    test_stack_does_not_move_under_deep_recursion();
    test_deep_recursion_preserves_live_frames();
    test_registers_are_slot_granular();

    printf("stack_test: all tests passed\n");

    return 0;
}
