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
} CodegenState;

static void codegen_statement(CodegenState *state, ASTStmt *ast);
static unsigned int codegen_expression(CodegenState *state, ASTExpr *ast);
static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node);

static unsigned int codegen_alloc_register(CodegenState *state) {
    assert(state->next_reg < VM_MAX_REGISTERS);

    return state->next_reg++;
}

static void codegen_reset_registers(CodegenState *state) { state->next_reg = state->first_reg; }

Chunk *codegen_generate(ASTScript *script) {
    CodegenState state = {
        .chunk = chunk_create(),
        .first_reg = script->vars_count,
        .next_reg = script->vars_count,
    };

    for (int i = 0; i < script->statements.size; i++) {
        codegen_statement(&state, script->statements.data[i]);
    }

    return state.chunk;
}

typedef struct {
    size_t position;
    unsigned int cond_reg;
} PatchLabel;

static void codegen_statement(CodegenState *state, ASTStmt *ast) {
    switch (ast->type) {
    case STMT_EXPR:
        codegen_expression(state, ast->expr.value);
        break;
    case STMT_RETURN: {
        unsigned int reg = codegen_expression(state, ast->ret.result);
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));
        break;
    }
    case STMT_VAR_DECL: {
        if (ast->var_decl.initializer) {
            unsigned int r1 = codegen_expression(state, ast->var_decl.initializer);
            chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, ast->var_decl.reg, r1, 0));
        }
        break;
    }
    case STMT_ASSIGN: {
        unsigned int rd = codegen_expression(state, ast->assign.target);
        unsigned int r1 = codegen_expression(state, ast->assign.value);

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, rd, r1, 0));
        break;
    }
    case STMT_BLOCK: {
        for (int i = 0; i < ast->block.list.size; i++) {
            codegen_statement(state, ast->block.list.data[i]);
        }
        break;
    }
    case STMT_IF: {
        unsigned int cond_reg = codegen_expression(state, ast->ifstmt.condition);
        PatchLabel if_false = {
            .position = chunk_add_instruction(state->chunk, 0),
            .cond_reg = cond_reg,
        };

        codegen_reset_registers(state);

        codegen_statement(state, ast->ifstmt.then_block);

        if (!ast->ifstmt.else_block) {
            Instruction jmp_if_false =
                VM_ENCODE_I(OP_JMP_IF_FALSE, if_false.cond_reg, state->chunk->size - if_false.position - 1);
            chunk_patch_instruction(state->chunk, if_false.position, jmp_if_false);
            break;
        }

        PatchLabel end = {
            .position = chunk_add_instruction(state->chunk, 0),
        };

        Instruction jmp_if_false =
            VM_ENCODE_I(OP_JMP_IF_FALSE, if_false.cond_reg, state->chunk->size - if_false.position - 1);
        chunk_patch_instruction(state->chunk, if_false.position, jmp_if_false);

        codegen_statement(state, ast->ifstmt.else_block);

        Instruction jmp = VM_ENCODE_I(OP_JMP, 0, state->chunk->size - end.position - 1);
        chunk_patch_instruction(state->chunk, end.position, jmp);
        break;
    }
    }

    codegen_reset_registers(state);
}

static unsigned int codegen_expression(CodegenState *state, ASTExpr *ast) {
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

    unsigned int lhs = codegen_expression(state, node->bin_op.left);
    unsigned int rhs = codegen_expression(state, node->bin_op.right);

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
