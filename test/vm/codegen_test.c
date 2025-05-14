#include "ast.h"
#include "vm/codegen.h"
#include "vm/opcode.h"
#include <assert.h>

// Test number literal compilation
static void test_number() {
    ASTNode *num = new_ast_number_node(42);
    Chunk *chunk = codegen_generate(num);

    // Verify chunk contains:
    // 1. LOAD_CONST R0, [const_index]
    // 2. RETURN R0
    assert(chunk->instructions_size == 2);

    // Check LOAD_CONST instruction
    Instruction inst = chunk->instructions[0];
    assert((inst >> 26) == OP_LOAD_CONST); // Opcode
    assert(((inst >> 19) & 0x7F) == 0);    // Register R0

    // Check constant pool
    assert(chunk->const_pool->count == 1);
    assert(chunk->const_pool->constants[0].number == 42.0);

    chunk_free(chunk);
    ast_free(num);
}

// Test binary addition
static void test_bin_op_add() {
    ASTNode *left = new_ast_number_node(3.0);
    ASTNode *right = new_ast_number_node(4.0);
    ASTNode *add = new_ast_bin_op_node(left, BIN_OP_ADD, right);

    Chunk *chunk = codegen_generate(add);

    // Expected instructions:
    // 1. LOAD_CONST R0, [3.0]
    // 2. LOAD_CONST R1, [4.0]
    // 3. ADD R0, R0, R1
    // 4. RETURN R0
    assert(chunk->instructions_size == 4);

    // Verify ADD instruction
    Instruction inst = chunk->instructions[2];
    assert((inst >> 26) == OP_ADD);
    assert(((inst >> 19) & 0x7F) == 0); // Dest R0
    assert(((inst >> 12) & 0x7F) == 0); // Src1 R0
    assert(((inst >> 5) & 0x7F) == 1);  // Src2 R1

    chunk_free(chunk);
    ast_free(add);
}

int main(void) {
    test_number();
    test_bin_op_add();

    return 0;
}
