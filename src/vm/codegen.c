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

typedef unsigned int Register;

typedef struct {
    Chunk *chunk;
    Register next_reg;
    int decl_var_count;

    ValueList *global_data;
    FuncProtoList *global_funcs;

} CodegenState;

static void codegen_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_return_stmt(CodegenState *state, ASTReturnStmt *ast);
static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast);
static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast);
static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast);
static void codegen_if_stmt(CodegenState *state, ASTIfStmt *ast);
static void codegen_func_decl_stmt(CodegenState *state, ASTFuncDecl *ast);

static Register codegen_expr_alloc(CodegenState *state, ASTExpr *expr);
static void codegen_expr(CodegenState *state, ASTExpr *ast, Register reg);
static void codegen_literal_expr(CodegenState *state, ASTExpr *node, Register reg);
static void codegen_identifier_expr(CodegenState *state, ASTExpr *node, Register reg);
static void codegen_call_expr(CodegenState *state, ASTExpr *node, Register reg);
static void codegen_bin_op_expr(CodegenState *state, ASTExpr *node, Register reg);
static void codegen_bin_op_logical_expr(CodegenState *state, ASTExpr *node, Register reg);

static OpCode bin_op_to_float_op(BinOp bin_op);
static OpCode bin_op_to_int_op(BinOp bin_op);

static Register codegen_alloc_register(CodegenState *state);
static void codegen_free_register(CodegenState *state, Register reg);

typedef struct {
    size_t position;
    Register cond_reg;
} CodegenLabel;

CodegenLabel codegen_create_label(CodegenState *state);
void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, Register reg);

Chunk *codegen_generate(ASTScript *script, ValueList *global_data, FuncProtoList *global_funcs) {
    CodegenState state = {
        .chunk = chunk_create(),
        .next_reg = 0,
        .decl_var_count = 0,
        .global_data = global_data,
        .global_funcs = global_funcs,
    };

    for (int i = 0; i < script->statements.size; i++) {
        codegen_stmt(&state, script->statements.data[i]);
    }

    return state.chunk;
}

static void codegen_stmt(CodegenState *state, ASTStmt *ast) {
    switch (ast->kind) {
    case STMT_EXPR:
        codegen_expr_alloc(state, ast->expr.value);
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
    Register reg = codegen_expr_alloc(state, ast->result);
    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));
}

static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast) {
    if (ast->symbol->scope_depth > 0) {
        ast->symbol->offset = codegen_alloc_register(state);
        state->decl_var_count++;
    } else {
        Value value;
        value_list_add(state->global_data, value);
        ast->symbol->offset = state->global_data->size - 1;
    }

    if (!ast->initializer) {
        return;
    }

    if (ast->symbol->scope_depth > 0) {
        codegen_expr(state, ast->initializer, ast->symbol->offset);
        return;
    }

    Register r1 = codegen_expr_alloc(state, ast->initializer);
    Instruction store = VM_ENCODE_I(OP_STORE_GLOBAL, r1, ast->symbol->offset);
    chunk_add_instruction(state->chunk, store);

    codegen_free_register(state, r1);
}

static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast) {
    if (ast->target->symbol->scope_depth > 0) {
        codegen_expr(state, ast->value, ast->target->symbol->offset);
        return;
    }

    Register r1 = codegen_expr_alloc(state, ast->value);
    Instruction store = VM_ENCODE_I(OP_STORE_GLOBAL, r1, ast->target->symbol->offset);
    chunk_add_instruction(state->chunk, store);

    codegen_free_register(state, r1);
}

static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast) {
    for (int i = 0; i < ast->list.size; i++) {
        codegen_stmt(state, ast->list.data[i]);
    }
}

static void codegen_if_stmt(CodegenState *state, ASTIfStmt *ast) {
    Register cond_reg = codegen_expr_alloc(state, ast->condition);
    CodegenLabel if_false = codegen_create_label(state);

    codegen_free_register(state, cond_reg);

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

static void codegen_func_decl_stmt(CodegenState *state, ASTFuncDecl *ast) {
    Chunk *func_chunk = chunk_create();

    Register func_next_reg = 1;

    for (int i = 0; i < ast->params.size; i++) {
        ast->params.data[i]->symbol->offset = func_next_reg++;
    }

    CodegenState func_state = (CodegenState){
        .chunk = func_chunk,
        .next_reg = func_next_reg,
    };

    codegen_stmt(&func_state, ast->body);

    FuncPrototype proto = (FuncPrototype){
        .chunk = func_chunk,
        .arity = ast->params.size,
        .max_registers = func_state.next_reg,
    };

    if (VM_DECODE_OPCODE(instruction_list_back(&func_chunk->instructions)) != OP_RETURN) {
        chunk_add_instruction(func_chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));
    }

    func_proto_list_add(state->global_funcs, proto);
    ast->symbol->offset = state->global_funcs->size - 1;
}

static Register codegen_expr_alloc(CodegenState *state, ASTExpr *ast) {
    Register reg = codegen_alloc_register(state);

    codegen_expr(state, ast, reg);

    return reg;
}

static void codegen_expr(CodegenState *state, ASTExpr *ast, Register reg) {
    switch (ast->kind) {
    case EXPR_LITERAL:
        codegen_literal_expr(state, ast, reg);
        break;
    case EXPR_IDENTIFIER:
        codegen_identifier_expr(state, ast, reg);
        break;
    case EXPR_CALL:
        codegen_call_expr(state, ast, reg);
        break;
    case EXPR_BIN_OP:
        codegen_bin_op_expr(state, ast, reg);
        break;
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

static void codegen_literal_expr(CodegenState *state, ASTExpr *node, Register reg) {
    Register const_index = constpool_add(state->chunk->const_pool, value_from_literal(node->lit));
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, reg, const_index);

    chunk_add_instruction(state->chunk, load_const);
}

static void codegen_identifier_expr(CodegenState *state, ASTExpr *node, Register reg) {
    if (node->symbol->scope_depth > 0) {
        Instruction move = VM_ENCODE_R(OP_MOVE, reg, node->symbol->offset, 0);
        chunk_add_instruction(state->chunk, move);
        return;
    }

    Instruction load_global = VM_ENCODE_I(OP_LOAD_GLOBAL, reg, node->symbol->offset);
    chunk_add_instruction(state->chunk, load_global);
}

static void codegen_call_expr(CodegenState *state, ASTExpr *node, Register reg) {
    assert(node->symbol->scope_depth == 0 && "only global functions are supported by now");

    codegen_expr(state, node->call.target, reg);

    size_t arg_count = node->call.params.size;

    for (size_t i = 0; i < arg_count; i++) {
        ASTExpr *arg = ast_expr_list_get(&node->call.params, i);
        codegen_expr_alloc(state, arg);
    }

    Instruction call = VM_ENCODE_R(OP_CALL, reg, arg_count, 0);
    chunk_add_instruction(state->chunk, call);
}

static void codegen_bin_op_expr(CodegenState *state, ASTExpr *node, Register reg) {
    switch (node->bin_op.op) {
    case BIN_OP_AND:
    case BIN_OP_OR:
        return codegen_bin_op_logical_expr(state, node, reg);
    default:
        break;
    }

    codegen_expr(state, node->bin_op.left, reg);
    Register rhs = codegen_expr_alloc(state, node->bin_op.right);

    OpCode op_code = node->bin_op.left->type->kind == TYPE_FLOAT ? bin_op_to_float_op(node->bin_op.op)
                                                                 : bin_op_to_int_op(node->bin_op.op);

    Instruction instruction = VM_ENCODE_R(op_code, reg, reg, rhs);
    chunk_add_instruction(state->chunk, instruction);

    codegen_free_register(state, rhs);
}

static void codegen_bin_op_logical_expr(CodegenState *state, ASTExpr *node, Register reg) {
    codegen_expr(state, node->bin_op.left, reg);

    CodegenLabel short_circuit = codegen_create_label(state);

    OpCode jump_op = node->bin_op.op == BIN_OP_AND ? OP_JMP_IF_FALSE : OP_JMP_IF_TRUE;

    codegen_expr(state, node->bin_op.right, reg);

    codegen_patch_jump(state, short_circuit, jump_op, reg);
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

static Register codegen_alloc_register(CodegenState *state) {
    assert(state->next_reg < VM_MAX_REGISTERS);

    return state->next_reg++;
}

static void codegen_free_register(CodegenState *state, Register reg) {
    if (reg < state->decl_var_count) {
        return;
    }

    state->next_reg--;
}

CodegenLabel codegen_create_label(CodegenState *state) {
    return (CodegenLabel){.position = chunk_add_instruction(state->chunk, 0)};
}

void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, Register reg) {
    Instruction patch = VM_ENCODE_I(op, reg, state->chunk->instructions.size - label.position - 1);
    chunk_patch_instruction(state->chunk, label.position, patch);
}
