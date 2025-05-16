#include "codegen.h"
#include "ast.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include "vm/vm.h"
#include <assert.h>

typedef struct {
    Chunk *chunk;
    size_t next_register;
} CodegenState;

static void codegen_statement(CodegenState *state, ASTStmt *ast);
static size_t codegen_expression(CodegenState *state, ASTExpr *ast);
static size_t codegen_literal(CodegenState *state, ASTExpr *node);
static size_t codegen_bin_op(CodegenState *state, ASTExpr *node);

static size_t codegen_alloc_register(CodegenState *state);
static void codegen_free_register(CodegenState *state);

Chunk *codegen_generate(ASTScript *script) {
    CodegenState state = {
        .chunk = chunk_create(),
        .next_register = 0,
    };

    for (size_t i = 0; i < script->count; i++) {
        codegen_statement(&state, script->statements[i]);
    }

    return state.chunk;
}

static void codegen_statement(CodegenState *state, ASTStmt *ast) {
    switch (ast->type) {
    case STMT_EXPR:
        codegen_expression(state, ast->expr.value);
        break;
    case STMT_RETURN: {
        size_t reg = codegen_expression(state, ast->ret.result);
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));
        break;
    }
    case STMT_ASSIGN:
    case STMT_VAR_DECL:
        break;
    }
}

static size_t codegen_expression(CodegenState *state, ASTExpr *ast) {
    switch (ast->type) {
    case EXPR_LITERAL:
        return codegen_literal(state, ast);
    case EXPR_BIN_OP:
        return codegen_bin_op(state, ast);
    default:
        return 0;
    }
}

static size_t codegen_literal(CodegenState *state, ASTExpr *node) {
    size_t const_index = constpool_add(state->chunk->const_pool, node->literal.value);
    size_t reg = codegen_alloc_register(state);
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, reg, const_index);

    chunk_add_instruction(state->chunk, load_const);

    return reg;
}

static size_t codegen_bin_op(CodegenState *state, ASTExpr *node) {
    size_t lhs_reg = codegen_expression(state, node->bin_op.left);
    size_t rhs_reg = codegen_expression(state, node->bin_op.right);

    Instruction instruction;
    switch (node->bin_op.op) {
    case BIN_OP_ADD:
        instruction = VM_ENCODE_R(OP_ADD, lhs_reg, lhs_reg, rhs_reg);
        break;
    case BIN_OP_SUB:
        instruction = VM_ENCODE_R(OP_SUB, lhs_reg, lhs_reg, rhs_reg);
        break;
    case BIN_OP_MUL:
        instruction = VM_ENCODE_R(OP_MUL, lhs_reg, lhs_reg, rhs_reg);
        break;
    case BIN_OP_DIV:
        instruction = VM_ENCODE_R(OP_DIV, lhs_reg, lhs_reg, rhs_reg);
        break;
    }

    chunk_add_instruction(state->chunk, instruction);
    codegen_free_register(state);

    return lhs_reg;
}

static size_t codegen_alloc_register(CodegenState *state) {
    assert(state->next_register < VM_MAX_REGISTERS);

    size_t current_register = state->next_register;
    state->next_register += 1;

    return current_register;
}

static void codegen_free_register(CodegenState *state) {
    assert(state->next_register > 0);

    state->next_register -= 1;
}
