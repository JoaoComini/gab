#include "ast.h"
#include "variant.h"
#include "vm/codegen.h"
#include "vm/opcode.h"
#include "vm/vm.h"
#include <assert.h>

// Test number literal compilation
static void test_number() {
    Variant variant = {.type = VARIANT_NUMBER, .number = 42};
    ASTExpr *num = ast_literal_expr_create(variant);
    ASTStmt *stmt = ast_expr_stmt_create(num);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve_symbols(script);

    Chunk *chunk = codegen_generate(script);

    // Verify chunk contains:
    // 1. LOAD_CONST R0, [const_index]
    assert(chunk->instructions_size == 1);

    // Check LOAD_CONST instruction
    Instruction inst = chunk->instructions[0];
    assert(VM_DECODE_OPCODE(inst) == OP_LOAD_CONST); // Opcode

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
    ast_script_resolve_symbols(script);

    Chunk *chunk = codegen_generate(script);

    // Expected instructions:
    // 1. LOAD_CONST R0, [3.0]
    // 2. LOAD_CONST R1, [4.0]
    // 3. ADD R2, R0, R1
    assert(chunk->instructions_size == 3);

    // Verify ADD instruction
    Instruction inst = chunk->instructions[2];
    assert(VM_DECODE_OPCODE(inst) == OP_ADD);
    assert(VM_DECODE_R_RD(inst) == 2); // Dest R0
    assert(VM_DECODE_R_R1(inst) == 0); // Src1 R0
    assert(VM_DECODE_R_R2(inst) == 1); // Src2 R1

    chunk_free(chunk);
    ast_script_free(script);
}

static void test_return() {
    Variant var = {.type = VARIANT_NUMBER, .number = 3};
    ASTExpr *result = ast_literal_expr_create(var);
    ASTStmt *stmt = ast_return_stmt_create(result);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve_symbols(script);

    Chunk *chunk = codegen_generate(script);

    // Expected instructions:
    // 1. LOAD_CONST R0, [3.0]
    // 2, RETURN R0
    assert(chunk->instructions_size == 2);

    Instruction load = chunk->instructions[0];
    assert(VM_DECODE_OPCODE(load) == OP_LOAD_CONST);

    Instruction ret = chunk->instructions[1];
    assert(VM_DECODE_OPCODE(ret) == OP_RETURN);

    chunk_free(chunk);
    ast_script_free(script);
}

static void test_var_decl() {
    Variant var = {.type = VARIANT_NUMBER, .number = 3};
    ASTExpr *inititalizer = ast_literal_expr_create(var);
    ASTStmt *stmt = ast_var_decl_stmt_create("x", inititalizer);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve_symbols(script);

    Chunk *chunk = codegen_generate(script);

    assert(chunk->instructions_size == 2);

    // Expected instructions:
    // 1. LOAD_CONST R1, [3.0]
    // 2. MOVE R0, R1

    Instruction load = chunk->instructions[0];
    assert(VM_DECODE_OPCODE(load) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load) == 1);

    Instruction move = chunk->instructions[1];
    assert(VM_DECODE_OPCODE(move) == OP_MOVE);
    assert(VM_DECODE_R_RD(move) == 0);
    assert(VM_DECODE_R_R1(move) == 1);

    chunk_free(chunk);
    ast_script_free(script);
}

static void test_variable_access() {
    Variant three = {.type = VARIANT_NUMBER, .number = 3};
    ASTExpr *inititalizer = ast_literal_expr_create(three);
    ASTStmt *var_decl = ast_var_decl_stmt_create("x", inititalizer); // let x = 3;

    ASTExpr *target_expr = ast_variable_expr_create("x");
    Variant two = {.type = VARIANT_NUMBER, .number = 2};
    ASTExpr *value_expr = ast_literal_expr_create(two);
    ASTStmt *assign_stmt = ast_assign_stmt_create(target_expr, value_expr); // x = 2;

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, var_decl);
    ast_script_add_statement(script, assign_stmt);
    ast_script_resolve_symbols(script);

    Chunk *chunk = codegen_generate(script);

    assert(chunk->instructions_size == 4);

    // Expected instructions:
    // 1. LOAD_CONST R1, [3.0]
    // 2. MOVE R0, R1
    // 3. LOAD_CONST R1, [2.0]
    // 4. MOVE R0, R1

    Instruction load1 = chunk->instructions[0];
    assert(VM_DECODE_OPCODE(load1) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load1) == 1);

    Instruction move1 = chunk->instructions[1];
    assert(VM_DECODE_OPCODE(move1) == OP_MOVE);
    assert(VM_DECODE_R_RD(move1) == 0);
    assert(VM_DECODE_R_R1(move1) == 1);

    Instruction load2 = chunk->instructions[2];
    assert(VM_DECODE_OPCODE(load2) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load2) == 1);

    Instruction move2 = chunk->instructions[3];
    assert(VM_DECODE_OPCODE(move2) == OP_MOVE);
    assert(VM_DECODE_R_RD(move2) == 0);
    assert(VM_DECODE_R_R1(move2) == 1);

    chunk_free(chunk);
    ast_script_free(script);
}

int main(void) {
    test_number();
    test_bin_op_add();
    test_return();
    test_var_decl();
    test_variable_access();

    return 0;
}
