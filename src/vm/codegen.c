#include "codegen.h"

#include "ast/ast.h"
#include "ast/expr.h"
#include "ast/stmt.h"
#include "scope.h"
#include "type.h"
#include "vm/chunk.h"
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
static void codegen_return_stmt(CodegenState *state, ASTReturnStmt *ast);
static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast);
static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast);
static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast);
static void codegen_if_stmt(CodegenState *state, ASTIfStmt *ast);
static void codegen_func_decl_stmt(CodegenState *state, ASTFuncDecl *ast);

static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast);
static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_logical_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_variable_expr(CodegenState *state, ASTExpr *node);

static OpCode bin_op_to_float_op(BinOp bin_op);
static OpCode bin_op_to_int_op(BinOp bin_op);

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
        .first_reg = script->vars_count + 1,
        .next_reg = script->vars_count + 1,
        .snapshot_index = 0,
    };

    for (int i = 0; i < script->statements.size; i++) {
        codegen_stmt(&state, script->statements.data[i]);
    }

    return state.chunk;
}

static void codegen_stmt(CodegenState *state, ASTStmt *ast) {
    switch (ast->kind) {
    case STMT_EXPR:
        codegen_expr(state, ast->expr.value);
        break;
    case STMT_RETURN: {
        codegen_return_stmt(state, &ast->ret);
        break;
    }
    case STMT_VAR_DECL: {
        codegen_var_decl_stmt(state, &ast->var_decl);
        break;
    }
    case STMT_ASSIGN: {
        codegen_assign_stmt(state, &ast->assign);
        break;
    }
    case STMT_BLOCK: {
        codegen_block_stmt(state, &ast->block);
        break;
    }
    case STMT_IF: {
        codegen_if_stmt(state, &ast->ifstmt);
        break;
    }
    case STMT_FUNC_DECL:
        codegen_func_decl_stmt(state, &ast->func_decl);
        break;
    }
}

static void codegen_return_stmt(CodegenState *state, ASTReturnStmt *ast) {
    codegen_save_snapshot(state);

    unsigned int reg = codegen_expr(state, ast->result);
    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));

    codegen_load_snapshot(state);
}

static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast) {
    if (!ast->initializer) {
        return;
    }

    codegen_save_snapshot(state);

    Instruction instruction;
    if (ast->symbol.scope == SCOPE_GLOBAL) {
        unsigned int r1 = codegen_expr(state, ast->initializer);
        instruction = VM_ENCODE_I(OP_STORE_GLOBAL, r1, ast->symbol.offset);
    } else {
        unsigned int r1 = codegen_expr(state, ast->initializer);
        instruction = VM_ENCODE_R(OP_MOVE, ast->symbol.offset + 1, r1, 0);
    }

    chunk_add_instruction(state->chunk, instruction);
    codegen_load_snapshot(state);
}

static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast) {
    codegen_save_snapshot(state);

    Instruction instruction;
    if (ast->target->kind == EXPR_VARIABLE && ast->target->symbol.scope == SCOPE_GLOBAL) {
        unsigned int r1 = codegen_expr(state, ast->value);
        instruction = VM_ENCODE_I(OP_STORE_GLOBAL, r1, ast->target->symbol.offset);
    } else {
        unsigned int rd = codegen_expr(state, ast->target);
        unsigned int r1 = codegen_expr(state, ast->value);

        instruction = VM_ENCODE_R(OP_MOVE, rd, r1, 0);
    }

    chunk_add_instruction(state->chunk, instruction);
    codegen_load_snapshot(state);
}

static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast) {
    for (int i = 0; i < ast->list.size; i++) {
        codegen_stmt(state, ast->list.data[i]);
    }
}

static void codegen_if_stmt(CodegenState *state, ASTIfStmt *ast) {
    unsigned int saved_reg = state->next_reg;

    unsigned int cond_reg = codegen_expr(state, ast->condition);
    CodegenLabel if_false = codegen_create_label(state);

    state->next_reg = saved_reg;

    codegen_stmt(state, ast->then_block);

    if (!ast->else_block) {
        codegen_patch_jump(state, if_false, OP_JMP_IF_FALSE, cond_reg);
        return;
    }

    CodegenLabel end = codegen_create_label(state);

    codegen_patch_jump(state, if_false, OP_JMP_IF_FALSE, cond_reg);

    codegen_stmt(state, ast->else_block);

    codegen_patch_jump(state, end, OP_JMP, 0);
}

static void codegen_func_decl_stmt(CodegenState *state, ASTFuncDecl *ast) {}

static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast) {
    switch (ast->kind) {
    case EXPR_LITERAL:
        return codegen_literal_expr(state, ast);
    case EXPR_VARIABLE:
        return codegen_variable_expr(state, ast);
    case EXPR_BIN_OP:
        return codegen_bin_op_expr(state, ast);
    }
}

static Value value_from_literal(Literal lit) {
    switch (lit.kind) {
    case TYPE_INT:
        return (Value){.type = TYPE_INT, .as_int = lit.as_int};
    case TYPE_FLOAT:
        return (Value){.type = TYPE_FLOAT, .as_float = lit.as_float};
    case TYPE_BOOL:
        return (Value){.type = TYPE_BOOL, .as_int = lit.as_int};
    default:
        break;
    }

    assert(0 && "unknown type");
}

static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node) {
    unsigned int const_index = constpool_add(state->chunk->const_pool, value_from_literal(node->lit));
    unsigned int r1 = codegen_alloc_register(state);
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, r1, const_index);

    chunk_add_instruction(state->chunk, load_const);

    return r1;
}

static unsigned int codegen_variable_expr(CodegenState *state, ASTExpr *node) {
    if (node->symbol.scope == SCOPE_LOCAL) {
        return node->symbol.offset + 1;
    }

    unsigned int rd = codegen_alloc_register(state);
    Instruction load_global = VM_ENCODE_I(OP_LOAD_GLOBAL, rd, node->symbol.offset);

    chunk_add_instruction(state->chunk, load_global);

    return rd;
}

static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node) {
    switch (node->bin_op.op) {
    case BIN_OP_AND:
    case BIN_OP_OR:
        return codegen_bin_op_logical_expr(state, node);
    default:
        break;
    }

    unsigned int saved_reg = state->next_reg;

    unsigned int lhs = codegen_expr(state, node->bin_op.left);
    unsigned int rhs = codegen_expr(state, node->bin_op.right);

    OpCode op_code = node->bin_op.left->type->kind == TYPE_FLOAT ? bin_op_to_float_op(node->bin_op.op)
                                                                 : bin_op_to_int_op(node->bin_op.op);

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

static unsigned int codegen_bin_op_logical_expr(CodegenState *state, ASTExpr *node) {
    unsigned int saved_reg = state->next_reg;

    unsigned int lhs = codegen_expr(state, node->bin_op.left);

    CodegenLabel short_circuit = codegen_create_label(state);

    OpCode jump_op = node->bin_op.op == BIN_OP_AND ? OP_JMP_IF_FALSE : OP_JMP_IF_TRUE;

    state->next_reg = saved_reg;

    unsigned int rhs = codegen_expr(state, node->bin_op.right);

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

    if (rhs != result) {
        Instruction instruction = VM_ENCODE_R(OP_MOVE, result, rhs, 0);
        chunk_add_instruction(state->chunk, instruction);
    }

    codegen_patch_jump(state, short_circuit, jump_op, lhs);

    state->next_reg = result + 1;

    return result;
}

static OpCode bin_op_to_float_op(BinOp bin_op) {
    switch (bin_op) {
    case BIN_OP_ADD:
        return OP_ADDF;
    case BIN_OP_SUB:
        return OP_SUBF;
    case BIN_OP_MUL:
        return OP_MULF;
    case BIN_OP_DIV:
        return OP_DIVF;
    case BIN_OP_LESS:
        return OP_CMP_LTF;
    case BIN_OP_GREATER:
        return OP_CMP_GTF;
    case BIN_OP_EQUAL:
        return OP_CMP_EQF;
    case BIN_OP_NEQUAL:
        return OP_CMP_NEF;
    case BIN_OP_LEQUAL:
        return OP_CMP_LEF;
    case BIN_OP_GEQUAL:
        return OP_CMP_GEF;
    default:
        assert(0 && "not a float operation");
    }
}

static OpCode bin_op_to_int_op(BinOp bin_op) {
    switch (bin_op) {
    case BIN_OP_ADD:
        return OP_ADDI;
    case BIN_OP_SUB:
        return OP_SUBI;
    case BIN_OP_MUL:
        return OP_MULI;
    case BIN_OP_DIV:
        return OP_DIVI;
    case BIN_OP_LESS:
        return OP_CMP_LTI;
    case BIN_OP_GREATER:
        return OP_CMP_GTI;
    case BIN_OP_EQUAL:
        return OP_CMP_EQI;
    case BIN_OP_NEQUAL:
        return OP_CMP_NEI;
    case BIN_OP_LEQUAL:
        return OP_CMP_LEI;
    case BIN_OP_GEQUAL:
        return OP_CMP_GEI;
    default:
        assert(0 && "not an int operation");
    }
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
    Instruction patch = VM_ENCODE_I(op, reg, state->chunk->instructions.size - label.position - 1);
    chunk_patch_instruction(state->chunk, label.position, patch);
}
