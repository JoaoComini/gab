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
#include <stdlib.h>

typedef struct {
    Chunk *chunk;
    unsigned int next_reg;

    ValueList *global_data;
    FuncProtoList *global_funcs;

    Diagnostics *diagnostics;
    bool failed;
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
static unsigned int codegen_call_expr(CodegenState *state, ASTExpr *node);

static OpCode bin_op_to_float_op(BinOp bin_op);
static OpCode bin_op_to_int_op(BinOp bin_op);

static unsigned int codegen_alloc_register(CodegenState *state, Span span);

typedef struct {
    size_t position;
    unsigned int cond_reg;
} CodegenLabel;

CodegenLabel codegen_create_label(CodegenState *state);
void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg);

Chunk *codegen_generate(ASTScript *script, ValueList *global_data, FuncProtoList *global_funcs,
                        Diagnostics *diagnostics, unsigned int *max_registers) {
    CodegenState state = {
        .chunk = chunk_create(),
        .next_reg = 0,
        .global_data = global_data,
        .global_funcs = global_funcs,
        .diagnostics = diagnostics,
        .failed = false,
    };

    for (int i = 0; i < script->statements.size; i++) {
        codegen_stmt(&state, script->statements.data[i]);
    }

    if (state.failed) {
        chunk_free(state.chunk);
        return NULL;
    }

    if (max_registers) {
        *max_registers = state.next_reg;
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
    case STMT_STRUCT_DECL:
        // Types are resolved at compile time and emit no code.
        break;
    }
}

static void codegen_return_stmt(CodegenState *state, ASTReturnStmt *ast) {
    unsigned int reg = codegen_expr(state, ast->result);
    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));
}

static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast) {
    if (ast->symbol->scope_depth == 0) {
        Value value;
        value_list_add(state->global_data, value);
        ast->symbol->offset = state->global_data->size - 1;
    } else {
        ast->symbol->offset =
            codegen_alloc_register(state, ast->initializer ? ast->initializer->span : (Span){0});
    }

    if (!ast->initializer) {
        return;
    }

    unsigned int r1 = codegen_expr(state, ast->initializer);

    Instruction instruction;
    if (ast->symbol->scope_depth == 0) {
        instruction = VM_ENCODE_I(OP_STORE_GLOBAL, r1, ast->symbol->offset);

    } else {
        instruction = VM_ENCODE_R(OP_MOVE, ast->symbol->offset, r1, 0);
    }

    chunk_add_instruction(state->chunk, instruction);
}

static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast) {
    Instruction instruction;
    if (ast->target->kind == EXPR_VARIABLE && ast->target->symbol->scope_depth == 0) {
        unsigned int r1 = codegen_expr(state, ast->value);
        instruction = VM_ENCODE_I(OP_STORE_GLOBAL, r1, ast->target->symbol->offset);
    } else {
        unsigned int rd = codegen_expr(state, ast->target);
        unsigned int r1 = codegen_expr(state, ast->value);

        instruction = VM_ENCODE_R(OP_MOVE, rd, r1, 0);
    }

    chunk_add_instruction(state->chunk, instruction);
}

static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast) {
    for (int i = 0; i < ast->list.size; i++) {
        codegen_stmt(state, ast->list.data[i]);
    }
}

static void codegen_if_stmt(CodegenState *state, ASTIfStmt *ast) {
    unsigned int cond_reg = codegen_expr(state, ast->condition);
    CodegenLabel if_false = codegen_create_label(state);

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

    // The slot is reserved before the body is generated so that a recursive
    // call inside it encodes a valid prototype index; the entry is filled in
    // once codegen knows the chunk and register count.
    func_proto_list_add(state->global_funcs, (FuncPrototype){0});
    size_t proto_index = state->global_funcs->size - 1;
    ast->symbol->offset = proto_index;

    unsigned int func_next_reg = 1;

    for (int i = 0; i < ast->params.size; i++) {
        ast->params.data[i]->symbol->offset = func_next_reg++;
    }

    CodegenState func_state = (CodegenState){
        .chunk = func_chunk,
        .next_reg = func_next_reg,
        .global_data = state->global_data,
        .global_funcs = state->global_funcs,
        .diagnostics = state->diagnostics,
        .failed = false,
    };

    codegen_stmt(&func_state, ast->body);

    state->failed = state->failed || func_state.failed;

    if (func_chunk->instructions.size == 0 ||
        VM_DECODE_OPCODE(instruction_list_back(&func_chunk->instructions)) != OP_RETURN) {
        chunk_add_instruction(func_chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));
    }

    func_proto_list_emplace(state->global_funcs, proto_index,
                            (FuncPrototype){
                                .chunk = func_chunk,
                                .arity = ast->params.size,
                                .max_registers = func_state.next_reg,
                            });
}

static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast) {
    switch (ast->kind) {
    case EXPR_LITERAL:
        return codegen_literal_expr(state, ast);
    case EXPR_VARIABLE:
        return codegen_variable_expr(state, ast);
    case EXPR_BIN_OP:
        return codegen_bin_op_expr(state, ast);
    case EXPR_CALL:
        return codegen_call_expr(state, ast);
    }

    assert(0 && "unknown expression kind");
    abort();
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
    abort();
}

static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node) {
    unsigned int const_index = constpool_add(state->chunk->const_pool, value_from_literal(node->lit));
    unsigned int r1 = codegen_alloc_register(state, node->span);
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, r1, const_index);

    chunk_add_instruction(state->chunk, load_const);

    return r1;
}

// Arguments go into the registers immediately above the destination, which is
// where the callee's frame expects them: its r0 is the return slot and its
// parameters are r1..arity.
static unsigned int codegen_call_expr(CodegenState *state, ASTExpr *node) {
    size_t arg_count = node->call.args.size;

    // dest and the argument slots must be contiguous, so they are reserved
    // before evaluating anything: an argument that is itself a call would
    // otherwise allocate its own registers in the middle of them.
    unsigned int dest = codegen_alloc_register(state, node->span);

    for (size_t i = 0; i < arg_count; i++) {
        codegen_alloc_register(state, node->call.args.data[i]->span);
    }

    for (size_t i = 0; i < arg_count; i++) {
        ASTExpr *arg = node->call.args.data[i];

        unsigned int arg_reg = codegen_expr(state, arg);

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, dest + 1 + i, arg_reg, 0));
    }

    Instruction call = VM_ENCODE_R(OP_CALL, dest, node->symbol->offset, arg_count);
    chunk_add_instruction(state->chunk, call);

    return dest;
}

static unsigned int codegen_variable_expr(CodegenState *state, ASTExpr *node) {
    if (node->symbol->scope_depth > 0) {
        return node->symbol->offset;
    }

    unsigned int rd = codegen_alloc_register(state, node->span);
    Instruction load_global = VM_ENCODE_I(OP_LOAD_GLOBAL, rd, node->symbol->offset);

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

    unsigned int lhs = codegen_expr(state, node->bin_op.left);
    unsigned int rhs = codegen_expr(state, node->bin_op.right);
    unsigned int result = codegen_alloc_register(state, node->span);

    OpCode op_code = node->bin_op.left->type->kind == TYPE_FLOAT ? bin_op_to_float_op(node->bin_op.op)
                                                                 : bin_op_to_int_op(node->bin_op.op);

    Instruction instruction = VM_ENCODE_R(op_code, result, lhs, rhs);
    chunk_add_instruction(state->chunk, instruction);

    return result;
}

static unsigned int codegen_bin_op_logical_expr(CodegenState *state, ASTExpr *node) {
    unsigned int lhs = codegen_expr(state, node->bin_op.left);

    CodegenLabel short_circuit = codegen_create_label(state);

    OpCode jump_op = node->bin_op.op == BIN_OP_AND ? OP_JMP_IF_FALSE : OP_JMP_IF_TRUE;

    unsigned int rhs = codegen_expr(state, node->bin_op.right);
    unsigned int result = codegen_alloc_register(state, node->span);

    Instruction move = VM_ENCODE_R(OP_MOVE, result, rhs, 0);
    chunk_add_instruction(state->chunk, move);

    codegen_patch_jump(state, short_circuit, jump_op, lhs);

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
        abort();
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
        abort();
    }
}

// Register exhaustion is a legitimate "program too complex" error rather than
// an internal invariant, so it is reported instead of asserted. Register 0 is
// returned as a placeholder; the failure flag stops the chunk being used.
static unsigned int codegen_alloc_register(CodegenState *state, Span span) {
    if (state->next_reg >= VM_MAX_REGISTERS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "expression too complex");
        }

        state->failed = true;
        return 0;
    }

    return state->next_reg++;
}

CodegenLabel codegen_create_label(CodegenState *state) {
    return (CodegenLabel){.position = chunk_add_instruction(state->chunk, 0)};
}

void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg) {
    Instruction patch = VM_ENCODE_I(op, reg, state->chunk->instructions.size - label.position - 1);
    chunk_patch_instruction(state->chunk, label.position, patch);
}
