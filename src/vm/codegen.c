#include "codegen.h"

#include "ast.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include "vm/vm.h"
#include <assert.h>

typedef struct {
    Chunk *chunk;
    unsigned int first_reg;
    unsigned int next_reg;

    unsigned int snapshot_stack[VM_MAX_REGISTERS];
    int snapshot_index;
} CodegenState;

static void codegen_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_return_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_var_decl_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_assign_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_block_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_if_stmt(CodegenState *state, ASTStmt *ast);

static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast);
static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node);

static unsigned int codegen_alloc_register(CodegenState *state);
static void codegen_save_snapshot(CodegenState *state);
static void codegen_load_snapshot(CodegenState *state);

typedef struct {
    size_t position;
    unsigned int cond_reg;
} CodegenLabel;

CodegenLabel codegen_create_label(CodegenState *state);
void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg);

Chunk *codegen_generate(ASTScript *script) {
    CodegenState state = {
        .chunk = chunk_create(),
        .first_reg = script->vars_count,
        .next_reg = script->vars_count,
        .snapshot_index = 0,
    };

    for (int i = 0; i < script->statements.size; i++) {
        codegen_stmt(&state, script->statements.data[i]);
    }

    return state.chunk;
}

static void codegen_stmt(CodegenState *state, ASTStmt *ast) {
    switch (ast->type) {
    case STMT_EXPR:
        codegen_expr(state, ast->expr.value);
        break;
    case STMT_RETURN: {
        codegen_return_stmt(state, ast);
        break;
    }
    case STMT_VAR_DECL: {
        codegen_var_decl_stmt(state, ast);
        break;
    }
    case STMT_ASSIGN: {
        codegen_assign_stmt(state, ast);
        break;
    }
    case STMT_BLOCK: {
        codegen_block_stmt(state, ast);
        break;
    }
    case STMT_IF: {
        codegen_if_stmt(state, ast);
        break;
    }
    }
}

static void codegen_return_stmt(CodegenState *state, ASTStmt *ast) {
    codegen_save_snapshot(state);

    unsigned int reg = codegen_expr(state, ast->ret.result);
    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));

    codegen_load_snapshot(state);
}

static void codegen_var_decl_stmt(CodegenState *state, ASTStmt *ast) {
    if (!ast->var_decl.initializer) {
        return;
    }

    codegen_save_snapshot(state);

    unsigned int r1 = codegen_expr(state, ast->var_decl.initializer);
    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, ast->var_decl.reg, r1, 0));

    codegen_load_snapshot(state);
}

static void codegen_assign_stmt(CodegenState *state, ASTStmt *ast) {
    codegen_save_snapshot(state);

    unsigned int rd = codegen_expr(state, ast->assign.target);
    unsigned int r1 = codegen_expr(state, ast->assign.value);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, rd, r1, 0));

    codegen_load_snapshot(state);
}

static void codegen_block_stmt(CodegenState *state, ASTStmt *ast) {
    for (int i = 0; i < ast->block.list.size; i++) {
        codegen_stmt(state, ast->block.list.data[i]);
    }
}

static void codegen_if_stmt(CodegenState *state, ASTStmt *ast) {
    unsigned int saved_reg = state->next_reg;

    unsigned int cond_reg = codegen_expr(state, ast->ifstmt.condition);
    CodegenLabel if_false = codegen_create_label(state);

    state->next_reg = saved_reg;

    codegen_stmt(state, ast->ifstmt.then_block);

    if (!ast->ifstmt.else_block) {
        codegen_patch_jump(state, if_false, OP_JMP_IF_FALSE, cond_reg);
        return;
    }

    CodegenLabel end = codegen_create_label(state);

    codegen_patch_jump(state, if_false, OP_JMP_IF_FALSE, cond_reg);

    codegen_stmt(state, ast->ifstmt.else_block);

    codegen_patch_jump(state, end, OP_JMP, 0);
}

static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast) {
    switch (ast->type) {
    case EXPR_LITERAL:
        return codegen_literal_expr(state, ast);
    case EXPR_BIN_OP:
        return codegen_bin_op_expr(state, ast);
    case EXPR_VARIABLE:
        return ast->symbol.reg;
    }
}

static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node) {
    unsigned int const_index = constpool_add(state->chunk->const_pool, node->literal.value);
    unsigned int r1 = codegen_alloc_register(state);
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, r1, const_index);

    chunk_add_instruction(state->chunk, load_const);

    return r1;
}

static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node) {
    unsigned int saved_reg = state->next_reg;

    unsigned int lhs = codegen_expr(state, node->bin_op.left);
    unsigned int rhs = codegen_expr(state, node->bin_op.right);

    OpCode op_code;
    switch (node->bin_op.op) {
    case BIN_OP_ADD:
        op_code = OP_ADD;
        break;
    case BIN_OP_SUB:
        op_code = OP_SUB;
        break;
    case BIN_OP_MUL:
        op_code = OP_MUL;
        break;
    case BIN_OP_DIV:
        op_code = OP_DIV;
        break;
    case BIN_OP_LESS:
        op_code = OP_CMP_LT;
        break;
    case BIN_OP_GREATER:
        op_code = OP_CMP_GT;
        break;
    case BIN_OP_EQUAL:
        op_code = OP_CMP_EQ;
        break;
    case BIN_OP_NEQUAL:
        op_code = OP_CMP_NE;
        break;
    case BIN_OP_LEQUAL:
        op_code = OP_CMP_LE;
        break;
    case BIN_OP_GEQUAL:
        op_code = OP_CMP_GE;
        break;
    }

    bool lhs_is_temp = lhs >= saved_reg;
    bool rhs_is_temp = rhs >= saved_reg;

    unsigned int result;
    if (lhs_is_temp) {
        result = lhs;
    } else if (rhs_is_temp) {
        result = rhs;
    } else { // if both registers aren't temporary
        result = codegen_alloc_register(state);
    }

    Instruction instruction = VM_ENCODE_R(op_code, result, lhs, rhs);
    chunk_add_instruction(state->chunk, instruction);

    state->next_reg = result + 1;

    return result;
}

static unsigned int codegen_alloc_register(CodegenState *state) {
    assert(state->next_reg < VM_MAX_REGISTERS);

    return state->next_reg++;
}

static void codegen_save_snapshot(CodegenState *state) {
    state->snapshot_stack[state->snapshot_index++] = state->next_reg;
}

static void codegen_load_snapshot(CodegenState *state) {
    state->next_reg = state->snapshot_stack[--state->snapshot_index];
}

CodegenLabel codegen_create_label(CodegenState *state) {
    return (CodegenLabel){.position = chunk_add_instruction(state->chunk, 0)};
}

void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg) {
    Instruction patch = VM_ENCODE_I(op, reg, state->chunk->size - label.position - 1);
    chunk_patch_instruction(state->chunk, label.position, patch);
}
