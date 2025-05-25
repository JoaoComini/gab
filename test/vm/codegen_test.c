#include "ast.h"
#include "string_ref.h"
#include "type.h"
#include "value.h"
#include "vm/codegen.h"
#include "vm/opcode.h"
#include "vm/vm.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>

// Test number literal compilation
static void test_number() {
    Literal lit = {.kind = TYPE_FLOAT, .as_float = 42.0};
    ASTExpr *num = ast_literal_expr_create(lit);
    ASTStmt *stmt = ast_expr_stmt_create(num);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve(script);

    Chunk *chunk = codegen_generate(script);

    // Verify chunk contains:
    // 1. LOAD_CONST R0, [const_index]
    assert(chunk->size == 1);

    // Check LOAD_CONST instruction
    Instruction inst = chunk->instructions[0];
    assert(VM_DECODE_OPCODE(inst) == OP_LOAD_CONST); // Opcode

    // Check constant pool
    assert(chunk->const_pool->count == 1);
    assert(chunk->const_pool->constants[0].as_float == 42.0);

    chunk_free(chunk);
    ast_script_free(script);
}

static void test_bin_op(OpCode expected_op, BinOp op) {
    Literal var_left = {.kind = TYPE_FLOAT, .as_float = 10};
    Literal var_right = {.kind = TYPE_FLOAT, .as_float = 5};

    ASTExpr *left = ast_literal_expr_create(var_left);
    ASTExpr *right = ast_literal_expr_create(var_right);
    ASTExpr *expr = ast_bin_op_expr_create(left, op, right);

    ASTStmt *stmt = ast_expr_stmt_create(expr);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve(script);

    Chunk *chunk = codegen_generate(script);

    assert(chunk->size == 3); // LOAD_CONST, LOAD_CONST, CMP

    Instruction inst = chunk->instructions[2];
    assert(VM_DECODE_OPCODE(inst) == expected_op);

    // R0 = 10, R1 = 5, R2 = result -> reusing R0 or R1 depends on the reg allocator
    unsigned int rd = VM_DECODE_R_RD(inst);
    unsigned int r1 = VM_DECODE_R_R1(inst);
    unsigned int r2 = VM_DECODE_R_R2(inst);

    // Check operand values from constant pool
    assert(chunk->const_pool->constants[0].as_float == 10.0);
    assert(chunk->const_pool->constants[1].as_float == 5.0);

    chunk_free(chunk);
    ast_script_free(script);
}

static void test_add() { test_bin_op(OP_ADDF, BIN_OP_ADD); }
static void test_sub() { test_bin_op(OP_SUBF, BIN_OP_SUB); }
static void test_mul() { test_bin_op(OP_MULF, BIN_OP_MUL); }
static void test_div() { test_bin_op(OP_DIVF, BIN_OP_DIV); }
static void test_cmp_equal() { test_bin_op(OP_CMP_EQF, BIN_OP_EQUAL); }
static void test_cmp_nequal() { test_bin_op(OP_CMP_NEF, BIN_OP_NEQUAL); }
static void test_cmp_less() { test_bin_op(OP_CMP_LTF, BIN_OP_LESS); }
static void test_cmp_lequal() { test_bin_op(OP_CMP_LEF, BIN_OP_LEQUAL); }
static void test_cmp_greater() { test_bin_op(OP_CMP_GTF, BIN_OP_GREATER); }
static void test_cmp_gequal() { test_bin_op(OP_CMP_GEF, BIN_OP_GEQUAL); }

static void test_return() {
    Literal var = {.kind = TYPE_FLOAT, .as_float = 3};
    ASTExpr *result = ast_literal_expr_create(var);
    ASTStmt *stmt = ast_return_stmt_create(result);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve(script);

    Chunk *chunk = codegen_generate(script);

    // Expected instructions:
    // 1. LOAD_CONST R0, [3.0]
    // 2, RETURN R0
    assert(chunk->size == 2);

    Instruction load = chunk->instructions[0];
    assert(VM_DECODE_OPCODE(load) == OP_LOAD_CONST);

    Instruction ret = chunk->instructions[1];
    assert(VM_DECODE_OPCODE(ret) == OP_RETURN);

    chunk_free(chunk);
    ast_script_free(script);
}

static void test_var_decl() {
    Literal var = {.kind = TYPE_FLOAT, .as_float = 3};
    ASTExpr *inititalizer = ast_literal_expr_create(var);

    StringRef ref = string_ref_create("x");
    ASTStmt *stmt = ast_var_decl_stmt_create(ref, NULL, inititalizer);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve(script);

    Chunk *chunk = codegen_generate(script);

    assert(chunk->size == 2);

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
    StringRef ref = string_ref_create("x");

    Literal three = {.kind = TYPE_FLOAT, .as_float = 3};
    ASTExpr *inititalizer = ast_literal_expr_create(three);
    ASTStmt *var_decl = ast_var_decl_stmt_create(ref, NULL, inititalizer); // let x = 3;

    ASTExpr *target_expr = ast_variable_expr_create(ref);
    Literal two = {.kind = TYPE_FLOAT, .as_float = 2};
    ASTExpr *value_expr = ast_literal_expr_create(two);
    ASTStmt *assign_stmt = ast_assign_stmt_create(target_expr, value_expr); // x = 2;

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, var_decl);
    ast_script_add_statement(script, assign_stmt);
    ast_script_resolve(script);

    Chunk *chunk = codegen_generate(script);

    assert(chunk->size == 4);

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

static void test_if_statement() {
    // Create condition: 10 > 5
    Literal var_left = {.kind = TYPE_FLOAT, .as_float = 10};
    Literal var_right = {.kind = TYPE_FLOAT, .as_float = 5};
    ASTExpr *left = ast_literal_expr_create(var_left);
    ASTExpr *right = ast_literal_expr_create(var_right);
    ASTExpr *cond = ast_bin_op_expr_create(left, BIN_OP_GREATER, right);

    // Create then block: return 1
    Literal then_val = {.kind = TYPE_FLOAT, .as_float = 1};
    ASTExpr *then_expr = ast_literal_expr_create(then_val);
    ASTStmt *then_stmt = ast_return_stmt_create(then_expr);

    ASTStmtList then_block_list = ast_stmt_list_create();
    ast_stmt_list_add(&then_block_list, then_stmt);
    ASTStmt *then_block = ast_block_stmt_create(then_block_list);

    // Create if statement
    ASTStmt *if_stmt = ast_if_stmt_create(cond, then_block, NULL);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, if_stmt);
    ast_script_resolve(script);

    Chunk *chunk = codegen_generate(script);

    // Expected instructions:
    // 1. LOAD_CONST R0, 10
    // 2. LOAD_CONST R1, 5
    // 3. CMP_GT R0, R0, R1
    // 4. JMP_IF_FALSE R0, +2 (skip then block)
    // 5. LOAD_CONST R1, 1
    // 6. RETURN R1
    assert(chunk->size == 6);

    // Verify condition
    Instruction load1 = chunk->instructions[0];
    assert(VM_DECODE_OPCODE(load1) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load1) == 0);

    Instruction load2 = chunk->instructions[1];
    assert(VM_DECODE_OPCODE(load2) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load2) == 1);

    Instruction cmp = chunk->instructions[2];
    assert(VM_DECODE_OPCODE(cmp) == OP_CMP_GTF);
    assert(VM_DECODE_R_RD(cmp) == 0);
    assert(VM_DECODE_R_R1(cmp) == 0);
    assert(VM_DECODE_R_R2(cmp) == 1);

    // Verify jump
    Instruction jmp = chunk->instructions[3];
    assert(VM_DECODE_OPCODE(jmp) == OP_JMP_IF_FALSE);
    assert(VM_DECODE_I_RD(jmp) == 0);  // Condition register
    assert(VM_DECODE_I_IMM(jmp) == 2); // Skip 2 instructions (to end)

    // Verify then block
    Instruction then_load = chunk->instructions[4];
    assert(VM_DECODE_OPCODE(then_load) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(then_load) == 0);

    Instruction ret = chunk->instructions[5];
    assert(VM_DECODE_OPCODE(ret) == OP_RETURN);
    assert(VM_DECODE_R_R1(ret) == 0);

    chunk_free(chunk);
    ast_script_free(script);
}

static void test_if_else_statement() {
    // Create condition: 5 > 10
    Literal var_left = {.kind = TYPE_FLOAT, .as_float = 5};
    Literal var_right = {.kind = TYPE_FLOAT, .as_float = 10};
    ASTExpr *left = ast_literal_expr_create(var_left);
    ASTExpr *right = ast_literal_expr_create(var_right);
    ASTExpr *cond = ast_bin_op_expr_create(left, BIN_OP_GREATER, right);

    // Create then block: return 1
    Literal then_val = {.kind = TYPE_FLOAT, .as_float = 1};
    ASTExpr *then_expr = ast_literal_expr_create(then_val);
    ASTStmt *then_stmt = ast_return_stmt_create(then_expr);
    ASTStmtList then_block_list = ast_stmt_list_create();
    ast_stmt_list_add(&then_block_list, then_stmt);
    ASTStmt *then_block = ast_block_stmt_create(then_block_list);

    // Create else block: return 0
    Literal else_val = {.kind = TYPE_FLOAT, .as_float = 0};
    ASTExpr *else_expr = ast_literal_expr_create(else_val);
    ASTStmt *else_stmt = ast_return_stmt_create(else_expr);
    ASTStmtList else_block_list = ast_stmt_list_create();
    ast_stmt_list_add(&else_block_list, else_stmt);
    ASTStmt *else_block = ast_block_stmt_create(else_block_list);

    // Create if-else statement
    ASTStmt *if_stmt = ast_if_stmt_create(cond, then_block, else_block);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, if_stmt);
    ast_script_resolve(script);

    Chunk *chunk = codegen_generate(script);

    // Expected instructions:
    // 1. LOAD_CONST R0, 5
    // 2. LOAD_CONST R1, 10
    // 3. CMP_GT R0, R0, R1
    // 4. JMP_IF_FALSE R0, +3 (skip to else block)
    // 5. LOAD_CONST R1, 1
    // 6. RETURN R1
    // 7. JMP +2 (skip else block)
    // 8. LOAD_CONST R1, 0
    // 9. RETURN R1
    assert(chunk->size == 9);

    // Verify condition
    Instruction cmp = chunk->instructions[2];
    assert(VM_DECODE_OPCODE(cmp) == OP_CMP_GTF);

    // Verify if-false jump to else block
    Instruction jmp_false = chunk->instructions[3];
    assert(VM_DECODE_OPCODE(jmp_false) == OP_JMP_IF_FALSE);
    assert(VM_DECODE_I_IMM(jmp_false) == 3); // Jump to else block

    // Verify unconditional jump over else block
    Instruction jmp = chunk->instructions[6];
    assert(VM_DECODE_OPCODE(jmp) == OP_JMP);
    assert(VM_DECODE_I_IMM(jmp) == 2); // Jump to end

    chunk_free(chunk);
    ast_script_free(script);
}

int main(void) {
    test_number();
    test_add();
    test_sub();
    test_mul();
    test_div();
    test_cmp_equal();
    test_cmp_nequal();
    test_cmp_less();
    test_cmp_lequal();
    test_cmp_greater();
    test_cmp_gequal();

    test_return();
    test_var_decl();
    test_variable_access();

    test_if_statement();
    test_if_else_statement();
    return 0;
}
