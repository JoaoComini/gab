#include "ast/ast.h"
#include "ast/stmt.h"
#include "scope.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "type.h"
#include "value.h"
#include "vm/chunk.h"
#include "vm/codegen.h"
#include "vm/opcode.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdlib.h>

// Test number literal compilation
static void test_number() {
    Literal lit = {.kind = TYPE_FLOAT, .as_float = 42.0};
    ASTExpr *num = ast_literal_expr_create(lit);
    ASTStmt *stmt = ast_expr_stmt_create(num);

    ValueList global_data = value_list_create();
    FuncProtoList global_funcs = func_proto_list_create();
    Scope *scope = scope_create(NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve(script, scope);

    Chunk *chunk = codegen_generate(script, &global_data, &global_funcs);

    // Verify chunk contains:
    // 1. LOAD_CONST R0, [const_index]
    assert(chunk->instructions.size == 1);

    // Check LOAD_CONST instruction
    Instruction inst = chunk->instructions.data[0];
    assert(VM_DECODE_OPCODE(inst) == OP_LOAD_CONST); // Opcode

    // Check constant pool
    assert(chunk->const_pool->count == 1);
    assert(chunk->const_pool->constants[0].as_float == 42.0);

    chunk_free(chunk);
    ast_script_destroy(script);
    scope_destroy(scope);
    func_proto_list_free(&global_funcs);
}

static void test_bin_op(OpCode expected_op, BinOp op) {
    Literal var_left = {.kind = TYPE_FLOAT, .as_float = 10};
    Literal var_right = {.kind = TYPE_FLOAT, .as_float = 5};

    ASTExpr *left = ast_literal_expr_create(var_left);
    ASTExpr *right = ast_literal_expr_create(var_right);
    ASTExpr *expr = ast_bin_op_expr_create(left, op, right);

    ASTStmt *stmt = ast_expr_stmt_create(expr);

    ValueList global_data = value_list_create();
    FuncProtoList global_funcs = func_proto_list_create();
    Scope *scope = scope_create(NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve(script, scope);

    Chunk *chunk = codegen_generate(script, &global_data, &global_funcs);

    assert(chunk->instructions.size == 3); // LOAD_CONST, LOAD_CONST, CMP

    Instruction inst = chunk->instructions.data[2];
    assert(VM_DECODE_OPCODE(inst) == expected_op);

    // R0 = 10, R1 = 5, R2 = result -> reusing R0 or R1 depends on the reg allocator
    unsigned int rd = VM_DECODE_R_RD(inst);
    unsigned int r1 = VM_DECODE_R_R1(inst);
    unsigned int r2 = VM_DECODE_R_R2(inst);

    // Check operand values from constant pool
    assert(chunk->const_pool->constants[0].as_float == 10.0);
    assert(chunk->const_pool->constants[1].as_float == 5.0);

    chunk_free(chunk);
    ast_script_destroy(script);
    scope_destroy(scope);
    func_proto_list_free(&global_funcs);
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

    Scope *scope = scope_create(NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve(script, scope);

    ValueList global_data = value_list_create();
    FuncProtoList global_funcs = func_proto_list_create();

    Chunk *chunk = codegen_generate(script, &global_data, &global_funcs);

    // Expected instructions:
    // 1. LOAD_CONST R0, [3.0]
    // 2, RETURN R0
    assert(chunk->instructions.size == 2);

    Instruction load = chunk->instructions.data[0];
    assert(VM_DECODE_OPCODE(load) == OP_LOAD_CONST);

    Instruction ret = chunk->instructions.data[1];
    assert(VM_DECODE_OPCODE(ret) == OP_RETURN);

    chunk_free(chunk);
    ast_script_destroy(script);
    scope_destroy(scope);
    func_proto_list_free(&global_funcs);
}

static void test_var_decl() {
    Literal var = {.kind = TYPE_FLOAT, .as_float = 3};
    ASTExpr *inititalizer = ast_literal_expr_create(var);

    StringRef ref = string_ref_create("x");
    ASTStmt *stmt = ast_var_decl_stmt_create(ref, NULL, inititalizer);

    Scope *scope = scope_create(NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    ast_script_resolve(script, scope);

    ValueList global_data = value_list_create();
    FuncProtoList global_funcs = func_proto_list_create();

    Chunk *chunk = codegen_generate(script, &global_data, &global_funcs);

    assert(chunk->instructions.size == 2);

    // let x = 3.0;
    // Expected instructions:
    // 1. LOAD_CONST R0, [3.0]
    // 2. STORE_GLOBAL IDX0, R0

    Instruction load = chunk->instructions.data[0];
    assert(VM_DECODE_OPCODE(load) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load) == 0);

    Instruction move = chunk->instructions.data[1];
    assert(VM_DECODE_OPCODE(move) == OP_STORE_GLOBAL);
    assert(VM_DECODE_I_RD(move) == 0);
    assert(VM_DECODE_I_IMM(move) == 0);

    chunk_free(chunk);
    ast_script_destroy(script);
    scope_destroy(scope);
    func_proto_list_free(&global_funcs);
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

    ValueList global_data = value_list_create();
    FuncProtoList global_funcs = func_proto_list_create();
    Scope *scope = scope_create(NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, var_decl);
    ast_script_add_statement(script, assign_stmt);
    ast_script_resolve(script, scope);

    Chunk *chunk = codegen_generate(script, &global_data, &global_funcs);

    assert(chunk->instructions.size == 4);

    // Expected instructions:
    // 1. LOAD_CONST R0, [3.0]
    // 2. STORE_GLOBAL IDX0, R0
    // 3. LOAD_CONST R1, [2.0]
    // 4. STORE_GLOBAL IDX0, R1

    Instruction load1 = chunk->instructions.data[0];
    assert(VM_DECODE_OPCODE(load1) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load1) == 0);

    Instruction move1 = chunk->instructions.data[1];
    assert(VM_DECODE_OPCODE(move1) == OP_STORE_GLOBAL);
    assert(VM_DECODE_I_RD(move1) == 0);
    assert(VM_DECODE_I_IMM(move1) == 0);

    Instruction load2 = chunk->instructions.data[2];
    assert(VM_DECODE_OPCODE(load2) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load2) == 1);

    Instruction move2 = chunk->instructions.data[3];
    assert(VM_DECODE_OPCODE(move2) == OP_STORE_GLOBAL);
    assert(VM_DECODE_I_RD(move2) == 1);
    assert(VM_DECODE_I_IMM(move2) == 0);

    chunk_free(chunk);
    ast_script_destroy(script);
    scope_destroy(scope);
    func_proto_list_free(&global_funcs);
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

    ValueList global_data = value_list_create();
    FuncProtoList global_funcs = func_proto_list_create();
    Scope *scope = scope_create(NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, if_stmt);
    ast_script_resolve(script, scope);

    Chunk *chunk = codegen_generate(script, &global_data, &global_funcs);

    // Expected instructions:
    // 1. LOAD_CONST R0, 10
    // 2. LOAD_CONST R1, 5
    // 3. CMP_GT R2, R0, R1
    // 4. JMP_IF_FALSE R2, +2 (skip then block)
    // 5. LOAD_CONST R3, 1
    // 6. RETURN R3
    assert(chunk->instructions.size == 6);

    // Verify condition
    Instruction load1 = chunk->instructions.data[0];
    assert(VM_DECODE_OPCODE(load1) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load1) == 0);

    Instruction load2 = chunk->instructions.data[1];
    assert(VM_DECODE_OPCODE(load2) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load2) == 1);

    Instruction cmp = chunk->instructions.data[2];
    assert(VM_DECODE_OPCODE(cmp) == OP_CMP_GTF);
    assert(VM_DECODE_R_RD(cmp) == 2);
    assert(VM_DECODE_R_R1(cmp) == 0);
    assert(VM_DECODE_R_R2(cmp) == 1);

    // Verify jump
    Instruction jmp = chunk->instructions.data[3];
    assert(VM_DECODE_OPCODE(jmp) == OP_JMP_IF_FALSE);
    assert(VM_DECODE_I_RD(jmp) == 2);  // Condition register
    assert(VM_DECODE_I_IMM(jmp) == 2); // Skip 2 instructions (to end)

    // Verify then block
    Instruction then_load = chunk->instructions.data[4];
    assert(VM_DECODE_OPCODE(then_load) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(then_load) == 3);

    Instruction ret = chunk->instructions.data[5];
    assert(VM_DECODE_OPCODE(ret) == OP_RETURN);
    assert(VM_DECODE_R_R1(ret) == 3);

    chunk_free(chunk);
    ast_script_destroy(script);
    scope_destroy(scope);
    func_proto_list_free(&global_funcs);
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

    ValueList global_data = value_list_create();
    FuncProtoList global_funcs = func_proto_list_create();
    Scope *scope = scope_create(NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, if_stmt);
    ast_script_resolve(script, scope);

    Chunk *chunk = codegen_generate(script, &global_data, &global_funcs);

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
    assert(chunk->instructions.size == 9);

    // Verify condition
    Instruction cmp = chunk->instructions.data[2];
    assert(VM_DECODE_OPCODE(cmp) == OP_CMP_GTF);

    // Verify if-false jump to else block
    Instruction jmp_false = chunk->instructions.data[3];
    assert(VM_DECODE_OPCODE(jmp_false) == OP_JMP_IF_FALSE);
    assert(VM_DECODE_I_IMM(jmp_false) == 3); // Jump to else block

    // Verify unconditional jump over else block
    Instruction jmp = chunk->instructions.data[6];
    assert(VM_DECODE_OPCODE(jmp) == OP_JMP);
    assert(VM_DECODE_I_IMM(jmp) == 2); // Jump to end

    chunk_free(chunk);
    ast_script_destroy(script);
    scope_destroy(scope);
    func_proto_list_free(&global_funcs);
}

static void test_func_decl() {
    StringRef int_str = string_ref_create("int");
    StringRef a_ref = string_ref_create("a");
    StringRef b_ref = string_ref_create("b");

    ASTField *param_a = ast_field_create(a_ref, type_spec_create(int_str));
    ASTField *param_b = ast_field_create(b_ref, type_spec_create(int_str));

    ASTFieldList params = ast_field_list_create();
    ast_field_list_add(&params, param_a);
    ast_field_list_add(&params, param_b);

    ASTExpr *a_var = ast_variable_expr_create(a_ref);
    ASTExpr *b_var = ast_variable_expr_create(b_ref);
    ASTExpr *add_expr = ast_bin_op_expr_create(a_var, BIN_OP_ADD, b_var);
    ASTStmt *return_stmt = ast_return_stmt_create(add_expr);

    ASTStmtList body_stmts = ast_stmt_list_create();
    ast_stmt_list_add(&body_stmts, return_stmt);

    ASTStmt *body = ast_block_stmt_create(body_stmts);

    StringRef func_ref = string_ref_create("add");
    ASTStmt *func = ast_func_decl_stmt_create(func_ref, type_spec_create(int_str), params, body);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, func);

    Scope global_scope;
    scope_init(&global_scope, NULL);

    ast_script_resolve(script, &global_scope);

    // 5. Set up codegen environment
    ValueList global_data = value_list_create();
    FuncProtoList global_funcs = func_proto_list_create();

    Chunk *chunk = codegen_generate(script, &global_data, &global_funcs);

    assert(chunk->instructions.size == 0);

    // 7. Verify results
    assert(global_funcs.size == 1);
    FuncPrototype *proto = &global_funcs.data[0];

    // Verify function prototype metadata
    assert(proto->arity == 2);
    assert(proto->max_registers == 4); // Params:1,2 + temp:3 + return:0

    // Verify instructions
    Chunk *proto_chunk = proto->chunk;
    assert(proto_chunk->instructions.size == 2);

    // Check ADD instruction
    Instruction add_instr = proto_chunk->instructions.data[0];
    assert(VM_DECODE_OPCODE(add_instr) == OP_ADDI);
    assert(VM_DECODE_R_RD(add_instr) == 3); // Temporary result
    assert(VM_DECODE_R_R1(add_instr) == 1); // Param a
    assert(VM_DECODE_R_R2(add_instr) == 2); // Param b

    // Check RETURN instruction
    Instruction ret_instr = proto_chunk->instructions.data[1];
    assert(VM_DECODE_OPCODE(ret_instr) == OP_RETURN);
    assert(VM_DECODE_R_R1(ret_instr) == 3); // Return value from ADD

    // 9. Cleanup
    chunk_free(chunk);
    ast_stmt_destroy(func);
    func_proto_list_free(&global_funcs);
    value_list_free(&global_data);
    scope_free(&global_scope);
}

int main(void) {
    string_init();

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

    test_func_decl();

    string_deinit();
    return 0;
}
