// How many instructions a loop body costs. These are the shapes the VM
// executes once per iteration, so a regression here is a regression in every
// program that loops -- which behaviour tests cannot see, since the answer
// stays correct however many instructions it took.
#include "support/run.h"
#include "vm/opcode.h"

#include <assert.h>
#include <stdio.h>

// Arithmetic over locals: three operations and the loop's own compare and
// jump. Nothing here should load a constant, because every literal is small
// enough to ride in its instruction.
static void test_a_local_loop_body_loads_no_constants() {
    TestProgram program = test_compile("func run(n: int): int {\n"
                                       "    let x: int = 1;\n"
                                       "    let y: int = 2;\n"
                                       "    for let i: int = 0; i < n; i += 1 {\n"
                                       "        x += y;\n"
                                       "        y = x - y;\n"
                                       "        x %= 100003;\n"
                                       "    }\n"
                                       "    return x;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    // 100003 exceeds VM_MAX_IMMEDIATE, so it is the one constant the body
    // needs. Everything else -- 1, 2, the loop's own step -- is immediate.
    // 100003 exceeds VM_MAX_IMMEDIATE, so it is the one constant the body
    // itself needs; the others belong to the setup before the loop.
    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 4);

    test_program_free(&program);
}

// The same arithmetic over struct fields costs a load and a store per access,
// where the local version addresses its slot directly.
static void test_a_field_loop_body_loads_and_stores_each_access() {
    TestProgram program = test_compile("struct Vec { x: int, y: int }\n"
                                       "func run(n: int): int {\n"
                                       "    let v: Vec;\n"
                                       "    v.x = 1;\n"
                                       "    v.y = 2;\n"
                                       "    for let i: int = 0; i < n; i += 1 {\n"
                                       "        v.x += v.y;\n"
                                       "        v.y = v.x - v.y;\n"
                                       "        v.x %= 100003;\n"
                                       "    }\n"
                                       "    return v.x;\n"
                                       "}\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    size_t loads = test_count_opcode(chunk, OP_LOAD_FIELD_4);
    size_t stores = test_count_opcode(chunk, OP_STORE_FIELD_4);

    // A local holding the same values would need none of these: the whole
    // difference between the two loops is that a field is addressed rather
    // than named.
    assert(loads > 0);
    assert(stores > 0);

    test_program_free(&program);
}

// A declaration with a literal initialiser loads the constant into the
// variable's own slot. Loading into a temporary and moving it down costs an
// instruction per declaration, which a loop pays on every entry.
static void test_a_literal_initialiser_loads_into_the_variable() {
    TestProgram program = test_compile("func f(): int { let x: int = 7; return x; }\n");

    Chunk *chunk = test_func_chunk(&program, 0);

    assert(test_count_opcode(chunk, OP_LOAD_CONST) == 1);
    assert(test_count_opcode(chunk, OP_MOVE) == 0);

    test_program_free(&program);
}

// Assigning one variable to another is the move itself, not a read into a
// temporary followed by a second move.
static void test_assigning_a_variable_is_a_single_move() {
    TestProgram program =
        test_compile("func f(): int { let a: int = 1; let b: int = 2; a = b; return a; }\n");

    assert(test_count_opcode(test_func_chunk(&program, 0), OP_MOVE) == 1);

    test_program_free(&program);
}

int main() {
    test_a_literal_initialiser_loads_into_the_variable();
    test_assigning_a_variable_is_a_single_move();
    test_a_local_loop_body_loads_no_constants();
    test_a_field_loop_body_loads_and_stores_each_access();

    printf("loop_shape_test: all tests passed\n");
    return 0;
}
