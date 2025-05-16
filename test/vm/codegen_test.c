#include "ast.h"
#include "variant.h"
#include "vm/codegen.h"
#include "vm/opcode.h"
#include <assert.h>

// Test number literal compilation
static void test_number() {
    Variant variant = {.type = VARIANT_NUMBER, .number = 42};
    ASTExpr *num = ast_literal_expr_create(variant);
    ASTStmt *stmt = ast_expr_stmt_create(num);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);

    Chunk *chunk = codegen_generate(script);

    // Verify chunk contains:
    // 1. LOAD_CONST R0, [const_index]
    assert(chunk->instructions_size == 1);

    // Check LOAD_CONST instruction
    Instruction inst = chunk->instructions[0];
    assert((inst >> 26) == OP_LOAD_CONST); // Opcode

    // Check constant pool
    assert(chunk->const_pool->count == 1);
    assert(chunk->const_pool->constants[0].number == 42.0);

    chunk_free(chunk);
    ast_script_free(script);
}

// Test binary addition
static void test_bin_op_add() {
    Variant var_left = {.type = VARIANT_NUMBER, .number = 3};
    Variant var_right = {.type = VARIANT_NUMBER, .number = 4};

    ASTExpr *left = ast_literal_expr_create(var_left);
    ASTExpr *right = ast_literal_expr_create(var_right);
    ASTExpr *add = ast_bin_op_expr_create(left, BIN_OP_ADD, right);
    ASTStmt *stmt = ast_expr_stmt_create(add);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);

    Chunk *chunk = codegen_generate(script);

    // Expected instructions:
    // 1. LOAD_CONST R0, [3.0]
    // 2. LOAD_CONST R1, [4.0]
    // 3. ADD R0, R0, R1
    assert(chunk->instructions_size == 3);

    // Verify ADD instruction
    Instruction inst = chunk->instructions[2];
    assert((inst >> 26) == OP_ADD);
    assert(((inst >> 19) & 0x7F) == 0); // Dest R0
    assert(((inst >> 12) & 0x7F) == 0); // Src1 R0
    assert(((inst >> 5) & 0x7F) == 1);  // Src2 R1

    chunk_free(chunk);
    ast_script_free(script);
}

static void test_return() {
    Variant var = {.type = VARIANT_NUMBER, .number = 3};
    ASTExpr *result = ast_literal_expr_create(var);
    ASTStmt *stmt = ast_return_stmt_create(result);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);

    Chunk *chunk = codegen_generate(script);

    // Expected instructions:
    // 1. LOAD_CONST R0, [3.0]
    // 1, RETURN R0
    assert(chunk->instructions_size == 2);

    Instruction load = chunk->instructions[0];
    assert((load >> 26) == OP_LOAD_CONST);

    Instruction ret = chunk->instructions[1];
    assert((ret >> 26) == OP_RETURN);

    chunk_free(chunk);
    ast_script_free(script);
}
int main(void) {
    test_number();
    test_bin_op_add();
    test_return();

    return 0;
}
