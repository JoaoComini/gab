#include "codegen.h"
#include "ast.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include "vm/variant.h"
#include "vm/vm.h"
#include <assert.h>

typedef struct {
    Chunk *chunk;
    size_t next_register;
} CodegenState;

static size_t codegen_expression(CodegenState *state, ASTNode *ast);
static size_t codegen_number(CodegenState *state, ASTNode *node);
static size_t codegen_bin_op(CodegenState *state, ASTNode *node);
static size_t codegen_variable(CodegenState *state, ASTNode *node);

static size_t codegen_alloc_register(CodegenState *state);
static void codegen_free_register(CodegenState *state);

Chunk *codegen_generate(ASTNode *node) {
    CodegenState state = {
        .chunk = chunk_create(),
        .next_register = 0,
    };

    size_t result_register = codegen_expression(&state, node);

    chunk_add_instruction(state.chunk, VM_ENCODE_R(OP_RETURN, 0, result_register, 0));

    return state.chunk;
}

static size_t codegen_expression(CodegenState *state, ASTNode *ast) {
    switch (ast->type) {
    case NODE_NUMBER:
        return codegen_number(state, ast);
    case NODE_BIN_OP:
        return codegen_bin_op(state, ast);
    case NODE_VARIABLE:
        return codegen_variable(state, ast);
    }
}

static size_t codegen_number(CodegenState *state, ASTNode *node) {
    Variant variant = {
        .type = VARIANT_NUMBER,
        .number = node->number,
    };

    size_t const_index = constpool_add(state->chunk->const_pool, variant);
    size_t reg = codegen_alloc_register(state);
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, reg, const_index);

    chunk_add_instruction(state->chunk, load_const);

    return reg;
}

static size_t codegen_bin_op(CodegenState *state, ASTNode *node) {
    size_t lhs_reg = codegen_expression(state, node->left);
    size_t rhs_reg = codegen_expression(state, node->right);

    Instruction instruction;
    switch (node->op) {
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

static size_t codegen_variable(CodegenState *state, ASTNode *node) {
    assert(0 && "Not implemented yet!");
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
