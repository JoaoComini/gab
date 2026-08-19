#include "support/run.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// The frame size codegen settled on for the given function, which is the number
// register reuse is supposed to hold flat as a function grows.
static int func_max_registers(VM *vm, size_t index) {
    assert(index < vm->global_funcs.size);

    return vm->global_funcs.data[index].max_registers;
}

static int compile_max_registers(const char *source, size_t index) {
    VM *vm = vm_create();

    vm_execute(vm, source);

    int result = func_max_registers(vm, index);

    vm_free(vm);

    return result;
}

// Statement temporaries are reclaimed, so frame size is set by the widest
// single statement rather than by the statement count.
static void test_frame_size_is_flat_in_statement_count() {
    int few = compile_max_registers("func f(n: int): int {\n"
                                    "let a = n + 1;\n"
                                    "let b = n + 2;\n"
                                    "return a + b;\n"
                                    "}\n",
                                    0);

    int many = compile_max_registers("func f(n: int): int {\n"
                                     "let a = n + 1;\n"
                                     "let b = n + 2;\n"
                                     "let c = n + 3;\n"
                                     "let d = n + 4;\n"
                                     "let e = n + 5;\n"
                                     "let g = n + 6;\n"
                                     "let h = n + 7;\n"
                                     "let i = n + 8;\n"
                                     "return a + b;\n"
                                     "}\n",
                                     0);

    // r0 return, r1 parameter, one slot per live local, plus the widest
    // statement's temporaries. Without reuse these were 9 and 27; one lower
    // than that again since 'n + 1' takes its literal as an immediate operand
    // rather than loading it into a register.
    assert(few == 5);
    assert(many == 11);

    // Six more locals cost six more slots, not three per statement.
    assert(many - few == 6);
}

// A monotonically rising floor gets this wrong: b and c are dead at the closing
// brace, so d must land back in the slot b used. Widening the dead block must
// therefore not widen the frame.
static void test_block_locals_are_reclaimed() {
    const char *two_inner = "func f(n: int): int {\n"
                            "let a = n + 1;\n"
                            "if n > 0 {\n"
                            "let b = n + 2;\n"
                            "let c = n + 3;\n"
                            "}\n"
                            "let d = n + 4;\n"
                            "return a + d;\n"
                            "}\n";

    const char *four_inner = "func f(n: int): int {\n"
                             "let a = n + 1;\n"
                             "if n > 0 {\n"
                             "let b = n + 2;\n"
                             "let c = n + 3;\n"
                             "}\n"
                             "if n > 0 {\n"
                             "let e = n + 5;\n"
                             "let g = n + 6;\n"
                             "}\n"
                             "let d = n + 4;\n"
                             "return a + d;\n"
                             "}\n";

    // The second arm's locals reuse the first arm's slots, so the two functions
    // need the same frame. Under a monotonic floor the second would need more.
    assert(compile_max_registers(two_inner, 0) == compile_max_registers(four_inner, 0));

    // And d reuses a slot the dead arm held rather than stacking on top of it.
    // Two lower than the register-only encoding: the arithmetic and the 'n > 0'
    // condition both take their literal as an immediate operand.
    assert(compile_max_registers(two_inner, 0) == 7);
}

// A long function used to exhaust the 127-slot frame at roughly three slots per
// statement. It must simply compile now.
static void test_many_statements_still_compile() {
    assert(test_run_int("func f(n: int): int {\n"
                        "let s01 = n + 1; let s02 = n + 1; let s03 = n + 1; let s04 = n + 1;\n"
                        "let s05 = n + 1; let s06 = n + 1; let s07 = n + 1; let s08 = n + 1;\n"
                        "let s09 = n + 1; let s10 = n + 1; let s11 = n + 1; let s12 = n + 1;\n"
                        "let s13 = n + 1; let s14 = n + 1; let s15 = n + 1; let s16 = n + 1;\n"
                        "let s17 = n + 1; let s18 = n + 1; let s19 = n + 1; let s20 = n + 1;\n"
                        "let s21 = n + 1; let s22 = n + 1; let s23 = n + 1; let s24 = n + 1;\n"
                        "let s25 = n + 1; let s26 = n + 1; let s27 = n + 1; let s28 = n + 1;\n"
                        "let s29 = n + 1; let s30 = n + 1; let s31 = n + 1; let s32 = n + 1;\n"
                        "let s33 = n + 1; let s34 = n + 1; let s35 = n + 1; let s36 = n + 1;\n"
                        "let s37 = n + 1; let s38 = n + 1; let s39 = n + 1; let s40 = n + 1;\n"
                        "let s41 = n + 1; let s42 = n + 1; let s43 = n + 1;\n"
                        "return s43;\n"
                        "}\n"
                        "func main(): int { return f(41); }\n"
                        "let r: int = main();") == 42);
}

// A statement made only of temporaries reuses the same slots every time, so a
// hundred of them cost no more than one.
static void test_expression_statements_reuse_slots() {
    int one = compile_max_registers("func f(n: int): int { n + 1; return n; }\n", 0);
    int several = compile_max_registers("func f(n: int): int { n + 1; n + 2; n + 3; n + 4; return n; }\n", 0);

    assert(one == several);
}

// The call's dest holds its result and must survive the reclamation that frees
// the argument block, or the surrounding expression reads a recycled slot.
static void test_call_result_survives_in_larger_expression() {
    assert(test_run_int("func add(a: int, b: int): int { return a + b; }\n"
                        "func main(): int { return add(1, 2) * 10 + add(3, 4); }\n"
                        "let r: int = main();") == 37);
}

// Nested calls as arguments: each inner call's dest is part of the outer call's
// argument block and must not be reclaimed before the outer call runs.
static void test_nested_call_results_survive() {
    assert(test_run_int("func add(a: int, b: int): int { return a + b; }\n"
                        "func main(): int { return add(add(add(1, 2), add(3, 4)), add(5, 6)); }\n"
                        "let r: int = main();") == 21);
}

// Builds a function of 'count' sequential 'let' statements and reports the
// frame codegen settled on, or -1 if the frame could not hold them.
static int compile_sequential_lets(unsigned int count) {
    size_t capacity = 64 + (size_t)count * 24;
    char *source = malloc(capacity);
    size_t used = (size_t)snprintf(source, capacity, "func f(n: int): int {\n");

    for (unsigned int i = 0; i < count; i++) {
        used += (size_t)snprintf(source + used, capacity - used, "let s%u = n + 1;\n", i);
    }

    // The script runs f so that a codegen failure is observable: a rejected
    // program never executes, leaving the result slot at its zeroed value.
    snprintf(source + used, capacity - used, "return n;\n}\nlet r: int = f(7);\n");

    VM *vm = vm_create();
    vm_execute(vm, source);

    int result = (*vm_slot(vm, 0)).as_int == 7 ? vm->global_funcs.data[0].max_registers : -1;

    vm_free(vm);
    free(source);

    return result;
}

// A function's frame is capped by the width of the register field. Each local
// costs one slot on top of r0 (the return slot), r1 (the parameter), and one
// temporary, so the ceiling shows up directly in the count of locals that fit.
//
// 'n + 1' takes its literal as an immediate operand, so it needs no register
// to hold the constant — which is why one more local fits than before.
static void test_frame_capacity() {
    // 252 locals fill the 255-slot frame exactly; 253 do not fit.
    assert(compile_sequential_lets(252) == 255);
    assert(compile_sequential_lets(253) == -1);

    // Under the previous 7-bit fields the ceiling was 127 slots. Both now
    // compile.
    assert(compile_sequential_lets(123) == 126);
    assert(compile_sequential_lets(124) == 127);
    assert(compile_sequential_lets(200) == 203);
}

int main() {
    test_frame_capacity();
    test_frame_size_is_flat_in_statement_count();
    test_block_locals_are_reclaimed();
    test_many_statements_still_compile();
    test_expression_statements_reuse_slots();
    test_call_result_survives_in_larger_expression();
    test_nested_call_results_survive();

    printf("register_reuse_test: all tests passed\n");

    return 0;
}
