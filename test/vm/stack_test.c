#include "compile.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool is_8_byte_aligned(const void *p) { return ((uintptr_t)p & 7u) == 0; }

static void test_stack_base_is_aligned_at_creation() {
    VM *vm = vm_create();

    assert(is_8_byte_aligned(vm->stack));
    assert(vm->stack == vm_registers(vm));

    vm_free(vm);
}

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

    int32_t result;
    memcpy(&result, vm_slot_at(vm, 0), sizeof(result));

    assert(result == 20100);

    vm_free(vm);
}

static void test_registers_are_slot_granular() {
    VM *vm = vm_create();

    uint8_t *regs = vm_registers(vm);

    vm_write_i32_at(regs, 0, 0x11111111);
    vm_write_i32_at(regs, 1, 0x22222222);

    assert(vm_read_i32_at(regs, 0) == 0x11111111);
    assert(vm_read_i32_at(regs, 1) == 0x22222222);
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
