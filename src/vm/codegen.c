#include "codegen.h"

#include "ast.h"
#include "symbol_table.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include "vm/scope.h"
#include "vm/vm.h"

#include <assert.h>

typedef struct {
    Chunk *chunk;
    Scope *scope;
    size_t next_register;
} CodegenState;

static void codegen_statement(CodegenState *state, ASTStmt *ast);
static size_t codegen_expression(CodegenState *state, ASTExpr *ast);
static size_t codegen_literal(CodegenState *state, ASTExpr *node);
static size_t codegen_bin_op(CodegenState *state, ASTExpr *node);

Chunk *codegen_generate(ASTScript *script) {
    CodegenState state = {
        .chunk = chunk_create(),
        .scope = scope_create(NULL),
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
    case STMT_VAR_DECL: {
        int rd = scope_alloc_register(state->scope);
        symbol_table_insert(state->scope->symbol_table, ast->var_decl.name, rd);

        if (ast->var_decl.initializer) {
            int r1 = codegen_expression(state, ast->var_decl.initializer);
            chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, rd, r1, 0));
        }
        break;
    }
    case STMT_ASSIGN:
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
    size_t reg = scope_alloc_register(state->scope);
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
    scope_free_register(state->scope);

    return lhs_reg;
}
