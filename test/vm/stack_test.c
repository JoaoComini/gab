#include "vm/vm.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

// Step 6's refcounted pointers need 8-byte alignment. A 4-byte-aligned base
// would put half the even slots on a 4-byte boundary, so the guarantee is on
// the base of the stack rather than on any particular slot.
static bool is_8_byte_aligned(const void *p) { return ((uintptr_t)p & 7u) == 0; }

static void test_stack_base_is_aligned_at_creation() {
    VM *vm = vm_create();

    assert(is_8_byte_aligned(vm->stack));
    assert(vm->stack == vm->registers);

    vm_free(vm);
}

// realloc does not preserve over-alignment, which is why growth allocates
// fresh. This runs a recursion deep enough to force the stack past its initial
// capacity and checks the base survived.
static void test_stack_base_stays_aligned_across_growth() {
    VM *vm = vm_create();

    const uint8_t *before = vm->stack;
    size_t capacity_before = vm->stack_capacity;

    vm_execute(vm, "func down(n: int): int {\n"
                   "if n <= 0 { return 0; }\n"
                   "let a = n + 1; let b = n + 2; let c = n + 3;\n"
                   "return down(n - 1) + a + b + c;\n"
                   "}\n"
                   "let r: int = down(200);\n");

    // The recursion must actually have moved the buffer, or this proves
    // nothing about growth.
    assert(vm->stack_capacity > capacity_before);
    assert(vm->stack != before);

    assert(is_8_byte_aligned(vm->stack));

    vm_free(vm);
}

// Growth copies the live frames rather than resizing in place, so a value
// written before a growth must still be readable after one.
static void test_growth_preserves_live_frames() {
    VM *vm = vm_create();

    vm_execute(vm, "func down(n: int): int {\n"
                   "if n <= 0 { return 0; }\n"
                   "let keep = n;\n"
                   "let rest = down(n - 1);\n"
                   "return keep + rest;\n"
                   "}\n"
                   "let r: int = down(200);\n");

    assert(vm->stack_capacity > 256);

    // 200 + 199 + ... + 1: every frame's 'keep' survived the reallocation.
    assert(vm_slot(vm, 0)->as_int == 20100);

    vm_free(vm);
}

// A register is a slot index scaled by sizeof(Value), so consecutive registers
// are exactly one slot apart in bytes.
static void test_registers_are_slot_granular() {
    VM *vm = vm_create();

    assert((uint8_t *)vm_reg(vm, 1) - (uint8_t *)vm_reg(vm, 0) == sizeof(Value));
    assert((uint8_t *)vm_slot(vm, 4) - vm->stack == 4 * sizeof(Value));

    vm_free(vm);
}

int main() {
    test_stack_base_is_aligned_at_creation();
    test_stack_base_stays_aligned_across_growth();
    test_growth_preserves_live_frames();
    test_registers_are_slot_granular();

    printf("stack_test: all tests passed\n");

    return 0;
}
