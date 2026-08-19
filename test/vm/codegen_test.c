#include "arena.h"
#include "ast/ast.h"
#include "ast/stmt.h"
#include "diagnostics.h"
#include "scope.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "support/test_context.h"
#include "type.h"
#include "value.h"
#include "vm/chunk.h"
#include "vm/codegen.h"
#include "vm/opcode.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static TestContext ctx;
static Arena *arena = NULL;

// These tests build the AST directly, so spans carry no meaning here.
#define TEST_SPAN ((Span){.line = 1, .column = 1})

// The sink is shared by every case; these tests all expect valid programs, so
// nothing should ever land in it.

static void assert_resolve(ASTScript *script, Scope *scope) {
    bool ok = ast_script_resolve(arena, script, scope, NULL, NULL, &ctx.diagnostics);

    if (!ok) {
        diagnostics_print(&ctx.diagnostics, stderr);
    }

    assert(ok);
}

static Chunk *assert_codegen(ASTScript *script, FuncProtoList *global_funcs) {
    Chunk *chunk = codegen_generate(script, global_funcs, &ctx.diagnostics, NULL);

    if (!chunk) {
        diagnostics_print(&ctx.diagnostics, stderr);
    }

    assert(chunk);

    return chunk;
}

// Test number literal compilation
static void test_number() {
    Literal lit = {.kind = TYPE_FLOAT, .as_float = 42.0};
    ASTExpr *num = ast_literal_expr_create(TEST_SPAN, lit);
    ASTStmt *stmt = ast_expr_stmt_create(TEST_SPAN, num);

    Scope *scope = scope_create(arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    assert_resolve(script, scope);

    FuncProtoList global_funcs = func_proto_list_create();
    Chunk *chunk = assert_codegen(script, &global_funcs);

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
    func_proto_list_free(&global_funcs);
}

static void test_bin_op(OpCode expected_op, BinOp op) {
    Literal var_left = {.kind = TYPE_FLOAT, .as_float = 10};
    Literal var_right = {.kind = TYPE_FLOAT, .as_float = 5};

    ASTExpr *left = ast_literal_expr_create(TEST_SPAN, var_left);
    ASTExpr *right = ast_literal_expr_create(TEST_SPAN, var_right);
    ASTExpr *expr = ast_bin_op_expr_create(TEST_SPAN, left, op, right);

    ASTStmt *stmt = ast_expr_stmt_create(TEST_SPAN, expr);

    Scope *scope = scope_create(arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    assert_resolve(script, scope);

    FuncProtoList global_funcs = func_proto_list_create();
    Chunk *chunk = assert_codegen(script, &global_funcs);

    assert(chunk->instructions.size == 3); // LOAD_CONST, LOAD_CONST, CMP

    Instruction inst = chunk->instructions.data[2];
    assert(VM_DECODE_OPCODE(inst) == expected_op);

    // Which registers the allocator picked is its business, but the compare
    // must read the two the loads wrote, and its operands must be distinct —
    // reading one register twice would compare a value with itself.
    unsigned int r1 = VM_DECODE_R_R1(inst);
    unsigned int r2 = VM_DECODE_R_R2(inst);

    assert(r1 != r2);
    assert(r1 == VM_DECODE_R_RD(chunk->instructions.data[0]));
    assert(r2 == VM_DECODE_R_RD(chunk->instructions.data[1]));

    // Check operand values from constant pool
    assert(chunk->const_pool->constants[0].as_float == 10.0);
    assert(chunk->const_pool->constants[1].as_float == 5.0);

    chunk_free(chunk);
    ast_script_destroy(script);
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

static void test_var_decl() {
    Literal var = {.kind = TYPE_FLOAT, .as_float = 3};
    ASTExpr *inititalizer = ast_literal_expr_create(TEST_SPAN, var);

    StringRef ref = string_ref_create("x");
    ASTStmt *stmt = ast_var_decl_stmt_create(TEST_SPAN, ref, NULL, inititalizer);

    Scope *scope = scope_create(arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, stmt);
    assert_resolve(script, scope);

    FuncProtoList global_funcs = func_proto_list_create();

    Chunk *chunk = assert_codegen(script, &global_funcs);

    assert(chunk->instructions.size == 2);

    // let x = 3.0;
    // x is a frame-zero local, so it owns R0 and the initializer is a
    // temporary above it.
    // Expected instructions:
    // 1. LOAD_CONST R1, [3.0]
    // 2. MOVE R0, R1

    Instruction load = chunk->instructions.data[0];
    assert(VM_DECODE_OPCODE(load) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load) == 1);

    Instruction move = chunk->instructions.data[1];
    assert(VM_DECODE_OPCODE(move) == OP_MOVE);
    assert(VM_DECODE_R_RD(move) == 0);
    assert(VM_DECODE_R_R1(move) == 1);

    chunk_free(chunk);
    ast_script_destroy(script);
    func_proto_list_free(&global_funcs);
}

static void test_variable_access() {
    StringRef ref = string_ref_create("x");

    Literal three = {.kind = TYPE_FLOAT, .as_float = 3};
    ASTExpr *inititalizer = ast_literal_expr_create(TEST_SPAN, three);
    ASTStmt *var_decl = ast_var_decl_stmt_create(TEST_SPAN, ref, NULL, inititalizer); // let x = 3;

    ASTExpr *target_expr = ast_variable_expr_create(TEST_SPAN, ref);
    Literal two = {.kind = TYPE_FLOAT, .as_float = 2};
    ASTExpr *value_expr = ast_literal_expr_create(TEST_SPAN, two);
    ASTStmt *assign_stmt = ast_assign_stmt_create(TEST_SPAN, target_expr, value_expr); // x = 2;

    FuncProtoList global_funcs = func_proto_list_create();
    Scope *scope = scope_create(arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, var_decl);
    ast_script_add_statement(script, assign_stmt);
    assert_resolve(script, scope);

    Chunk *chunk = assert_codegen(script, &global_funcs);

    assert(chunk->instructions.size == 4);

    // Expected instructions:
    // 1. LOAD_CONST R1, [3.0]
    // 2. MOVE R0, R1
    // 3. LOAD_CONST R1, [2.0]
    // 4. MOVE R0, R1
    //
    // x is a frame-zero local holding R0 for the whole script; both
    // temporaries land in R1, because each statement reclaims what it
    // allocated above x.

    Instruction load1 = chunk->instructions.data[0];
    assert(VM_DECODE_OPCODE(load1) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load1) == 1);

    Instruction move1 = chunk->instructions.data[1];
    assert(VM_DECODE_OPCODE(move1) == OP_MOVE);
    assert(VM_DECODE_R_RD(move1) == 0);
    assert(VM_DECODE_R_R1(move1) == 1);

    Instruction load2 = chunk->instructions.data[2];
    assert(VM_DECODE_OPCODE(load2) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(load2) == 1);

    Instruction move2 = chunk->instructions.data[3];
    assert(VM_DECODE_OPCODE(move2) == OP_MOVE);
    assert(VM_DECODE_R_RD(move2) == 0);
    assert(VM_DECODE_R_R1(move2) == 1);

    chunk_free(chunk);
    ast_script_destroy(script);
    func_proto_list_free(&global_funcs);
}

static void test_if_statement() {
    // Create condition: 10 > 5
    Literal var_left = {.kind = TYPE_FLOAT, .as_float = 10};
    Literal var_right = {.kind = TYPE_FLOAT, .as_float = 5};
    ASTExpr *left = ast_literal_expr_create(TEST_SPAN, var_left);
    ASTExpr *right = ast_literal_expr_create(TEST_SPAN, var_right);
    ASTExpr *cond = ast_bin_op_expr_create(TEST_SPAN, left, BIN_OP_GREATER, right);

    // Create then block: 1
    Literal then_val = {.kind = TYPE_FLOAT, .as_float = 1};
    ASTExpr *then_expr = ast_literal_expr_create(TEST_SPAN, then_val);
    ASTStmt *then_stmt = ast_expr_stmt_create(TEST_SPAN, then_expr);

    ASTStmtList then_block_list = ast_stmt_list_create();
    ast_stmt_list_add(&then_block_list, then_stmt);
    ASTStmt *then_block = ast_block_stmt_create(TEST_SPAN, then_block_list);

    // Create if statement
    ASTStmt *if_stmt = ast_if_stmt_create(TEST_SPAN, cond, then_block, NULL);

    FuncProtoList global_funcs = func_proto_list_create();
    Scope *scope = scope_create(arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, if_stmt);
    assert_resolve(script, scope);

    Chunk *chunk = assert_codegen(script, &global_funcs);

    // Expected instructions:
    // 1. LOAD_CONST R0, 10
    // 2. LOAD_CONST R1, 5
    // 3. CMP_GT R2, R0, R1
    // 4. JMP_IF_FALSE R2, +2 (skip then block)
    // 5. LOAD_CONST R3, 1
    assert(chunk->instructions.size == 5);

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
    assert(VM_DECODE_I_IMM(jmp) == 1); // Skip 1 instruction (to end)

    // Verify then block
    Instruction then_load = chunk->instructions.data[4];
    assert(VM_DECODE_OPCODE(then_load) == OP_LOAD_CONST);
    assert(VM_DECODE_I_RD(then_load) == 3);

    chunk_free(chunk);
    ast_script_destroy(script);
    func_proto_list_free(&global_funcs);
}

static void test_if_else_statement() {
    // Create condition: 5 > 10
    Literal var_left = {.kind = TYPE_FLOAT, .as_float = 5};
    Literal var_right = {.kind = TYPE_FLOAT, .as_float = 10};
    ASTExpr *left = ast_literal_expr_create(TEST_SPAN, var_left);
    ASTExpr *right = ast_literal_expr_create(TEST_SPAN, var_right);
    ASTExpr *cond = ast_bin_op_expr_create(TEST_SPAN, left, BIN_OP_GREATER, right);

    // Create then block: 1
    Literal then_val = {.kind = TYPE_FLOAT, .as_float = 1};
    ASTExpr *then_expr = ast_literal_expr_create(TEST_SPAN, then_val);
    ASTStmt *then_stmt = ast_expr_stmt_create(TEST_SPAN, then_expr);
    ASTStmtList then_block_list = ast_stmt_list_create();
    ast_stmt_list_add(&then_block_list, then_stmt);
    ASTStmt *then_block = ast_block_stmt_create(TEST_SPAN, then_block_list);

    // Create else block: 0
    Literal else_val = {.kind = TYPE_FLOAT, .as_float = 0};
    ASTExpr *else_expr = ast_literal_expr_create(TEST_SPAN, else_val);
    ASTStmt *else_stmt = ast_expr_stmt_create(TEST_SPAN, else_expr);
    ASTStmtList else_block_list = ast_stmt_list_create();
    ast_stmt_list_add(&else_block_list, else_stmt);
    ASTStmt *else_block = ast_block_stmt_create(TEST_SPAN, else_block_list);

    // Create if-else statement
    ASTStmt *if_stmt = ast_if_stmt_create(TEST_SPAN, cond, then_block, else_block);

    FuncProtoList global_funcs = func_proto_list_create();
    Scope *scope = scope_create(arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, if_stmt);
    assert_resolve(script, scope);

    Chunk *chunk = assert_codegen(script, &global_funcs);

    // Expected instructions:
    // 1. LOAD_CONST R0, 5
    // 2. LOAD_CONST R1, 10
    // 3. CMP_GT R0, R0, R1
    // 4. JMP_IF_FALSE R0, +3 (skip to else block)
    // 5. LOAD_CONST R1, 1
    // 6. JMP +2 (skip else block)
    // 7. LOAD_CONST R1, 0
    assert(chunk->instructions.size == 7);

    // Verify condition
    Instruction cmp = chunk->instructions.data[2];
    assert(VM_DECODE_OPCODE(cmp) == OP_CMP_GTF);

    // Verify if-false jump to else block
    Instruction jmp_false = chunk->instructions.data[3];
    assert(VM_DECODE_OPCODE(jmp_false) == OP_JMP_IF_FALSE);
    assert(VM_DECODE_I_IMM(jmp_false) == 2); // Jump to else block

    // Verify unconditional jump over else block
    Instruction jmp = chunk->instructions.data[5];
    assert(VM_DECODE_OPCODE(jmp) == OP_JMP);
    assert(VM_DECODE_I_IMM(jmp) == 1); // Jump to end

    chunk_free(chunk);
    ast_script_destroy(script);
    func_proto_list_free(&global_funcs);
}

static void test_func_decl() {
    StringRef int_str = string_ref_create("int");
    StringRef a_ref = string_ref_create("a");
    StringRef b_ref = string_ref_create("b");

    ASTField *param_a = ast_field_create(TEST_SPAN, a_ref, type_spec_create(int_str, 0));
    ASTField *param_b = ast_field_create(TEST_SPAN, b_ref, type_spec_create(int_str, 0));

    ASTFieldList params = ast_field_list_create();
    ast_field_list_add(&params, param_a);
    ast_field_list_add(&params, param_b);

    ASTExpr *a_var = ast_variable_expr_create(TEST_SPAN, a_ref);
    ASTExpr *b_var = ast_variable_expr_create(TEST_SPAN, b_ref);
    ASTExpr *add_expr = ast_bin_op_expr_create(TEST_SPAN, a_var, BIN_OP_ADD, b_var);
    ASTStmt *return_stmt = ast_return_stmt_create(TEST_SPAN, add_expr);

    ASTStmtList body_stmts = ast_stmt_list_create();
    ast_stmt_list_add(&body_stmts, return_stmt);

    ASTStmt *body = ast_block_stmt_create(TEST_SPAN, body_stmts);

    StringRef func_ref = string_ref_create("add");
    ASTStmt *func =
        ast_func_decl_stmt_create(TEST_SPAN, func_ref, NULL, type_spec_create(int_str, 0), params, body);

    ASTScript *script = ast_script_create();
    ast_script_add_statement(script, func);

    Scope global_scope;
    scope_init(&global_scope, arena, &ctx.strings, NULL);

    assert_resolve(script, &global_scope);

    // 5. Set up codegen environment
    FuncProtoList global_funcs = func_proto_list_create();

    Chunk *chunk = assert_codegen(script, &global_funcs);

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
    ast_script_destroy(script);
    func_proto_list_free(&global_funcs);
}

int main(void) {
    test_context_init(&ctx);
    arena = ctx.arena;

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

    test_var_decl();
    test_variable_access();

    test_if_statement();
    test_if_else_statement();

    test_func_decl();

    test_context_free(&ctx);
    return 0;
}
