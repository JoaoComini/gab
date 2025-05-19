#include "codegen.h"

#include "ast.h"
#include "symbol_table.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include "vm/vm.h"
#include <assert.h>

typedef struct {
    Chunk *chunk;
    unsigned int first_reg;
    unsigned int next_reg;
} CodegenState;

static void codegen_statement(CodegenState *state, ASTStmt *ast);
static unsigned int codegen_expression(CodegenState *state, ASTExpr *ast);
static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node);

static unsigned int codegen_alloc_register(CodegenState *state) {
    assert(state->next_reg < VM_MAX_REGISTERS);

    return state->next_reg++;
}

static void codegen_free_register(CodegenState *state) {
    if (state->next_reg <= state->first_reg) {
        return;
    }

    state->next_reg--;
}

Chunk *codegen_generate(ASTScript *script) {
    CodegenState state = {
        .chunk = chunk_create(),
        .first_reg = script->symbol_table->size,
        .next_reg = script->symbol_table->size,
    };

    for (int i = 0; i < script->statements_size; i++) {
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
        unsigned int reg = codegen_expression(state, ast->ret.result);
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));

        codegen_free_register(state);
        break;
    }
    case STMT_VAR_DECL: {
        if (ast->var_decl.initializer) {
            unsigned int r1 = codegen_expression(state, ast->var_decl.initializer);
            chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, ast->var_decl.reg, r1, 0));

            codegen_free_register(state);
        }
        break;
    }
    case STMT_ASSIGN: {
        unsigned int rd = codegen_expression(state, ast->assign.target);
        unsigned int r1 = codegen_expression(state, ast->assign.value);

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, rd, r1, 0));
        codegen_free_register(state);
        break;
    }
    }
}

static unsigned int codegen_expression(CodegenState *state, ASTExpr *ast) {
    switch (ast->type) {
    case EXPR_LITERAL:
        return codegen_literal_expr(state, ast);
    case EXPR_BIN_OP:
        return codegen_bin_op_expr(state, ast);
    case EXPR_VARIABLE:
        return ast->symbol.reg;
    default:
        return 0;
    }
}

static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node) {
    unsigned int const_index = constpool_add(state->chunk->const_pool, node->literal.value);
    unsigned int reg = codegen_alloc_register(state);
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, reg, const_index);

    chunk_add_instruction(state->chunk, load_const);

    return reg;
}

static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node) {
    unsigned int lhs_reg = codegen_expression(state, node->bin_op.left);
    unsigned int rhs_reg = codegen_expression(state, node->bin_op.right);
    unsigned int result_reg = codegen_alloc_register(state);

    Instruction instruction;
    switch (node->bin_op.op) {
    case BIN_OP_ADD:
        instruction = VM_ENCODE_R(OP_ADD, result_reg, lhs_reg, rhs_reg);
        break;
    case BIN_OP_SUB:
        instruction = VM_ENCODE_R(OP_SUB, result_reg, lhs_reg, rhs_reg);
        break;
    case BIN_OP_MUL:
        instruction = VM_ENCODE_R(OP_MUL, result_reg, lhs_reg, rhs_reg);
        break;
    case BIN_OP_DIV:
        instruction = VM_ENCODE_R(OP_DIV, result_reg, lhs_reg, rhs_reg);
        break;
    }

    chunk_add_instruction(state->chunk, instruction);
    codegen_free_register(state);
    codegen_free_register(state);

    return lhs_reg;
}
