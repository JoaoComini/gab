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

    // next_reg falls back at scope exits, so the frame is sized from the
    // highest slot ever reached rather than the final value.
    unsigned int max_reg;

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
static unsigned int codegen_variable_expr(ASTExpr *node);
static unsigned int codegen_call_expr(CodegenState *state, ASTExpr *node);

static OpCode bin_op_to_float_op(BinOp bin_op);
static OpCode bin_op_to_int_op(BinOp bin_op);

static unsigned int codegen_alloc_register(CodegenState *state, Span span);
static unsigned int codegen_alloc_slots(CodegenState *state, unsigned int count, unsigned int align_slots,
                                        Span span);
static void codegen_release_registers(CodegenState *state, unsigned int saved);
static unsigned int codegen_field_expr(CodegenState *state, ASTExpr *node);
static void codegen_store_field(CodegenState *state, ASTExpr *node, unsigned int src);
static unsigned int codegen_addr_of_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_deref_expr(CodegenState *state, ASTExpr *node);
static void codegen_store_deref(CodegenState *state, ASTExpr *node, unsigned int src);
static void codegen_copy_slots(CodegenState *state, unsigned int dest, unsigned int src, unsigned int count);

// A value occupies ceil(size / 4) consecutive slots, which is 1 for every
// scalar.
static unsigned int type_slot_count(const Type *type) {
    if (!type) {
        return 1;
    }

    return (unsigned int)((type->size + sizeof(Value) - 1) / sizeof(Value));
}

// Slot alignment a value of this type needs. Nothing wants more than one slot
// today; step 6's 8-byte pointer is the first case that will.
static unsigned int type_align_slots(const Type *type) {
    if (!type || type->alignment <= sizeof(Value)) {
        return 1;
    }

    return (unsigned int)(type->alignment / sizeof(Value));
}

static bool type_is_struct(const Type *type) { return type && type->kind == TYPE_STRUCT; }

// The field's width is known at compile time, so it picks the opcode instead
// of spending operand bits. 'load' selects the load or store family.
static OpCode field_opcode_for(size_t size, bool load, bool indirect, bool *ok) {
    *ok = true;

    switch (size) {
    case 1:
        if (indirect) {
            return load ? OP_LOAD_FIELD_PTR_1 : OP_STORE_FIELD_PTR_1;
        }
        return load ? OP_LOAD_FIELD_1 : OP_STORE_FIELD_1;
    case 2:
        if (indirect) {
            return load ? OP_LOAD_FIELD_PTR_2 : OP_STORE_FIELD_PTR_2;
        }
        return load ? OP_LOAD_FIELD_2 : OP_STORE_FIELD_2;
    case 4:
        if (indirect) {
            return load ? OP_LOAD_FIELD_PTR_4 : OP_STORE_FIELD_PTR_4;
        }
        return load ? OP_LOAD_FIELD_4 : OP_STORE_FIELD_4;
    default:
        break;
    }

    *ok = false;
    return load ? OP_LOAD_FIELD_4 : OP_STORE_FIELD_4;
}

typedef struct {
    size_t position;
    unsigned int cond_reg;
} CodegenLabel;

CodegenLabel codegen_create_label(CodegenState *state);
void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg);

Chunk *codegen_generate(ASTScript *script, FuncProtoList *global_funcs, Diagnostics *diagnostics,
                        unsigned int *max_registers) {
    CodegenState state = {
        .chunk = chunk_create(),
        .next_reg = 0,
        .max_reg = 0,
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
        *max_registers = state.max_reg;
    }

    return state.chunk;
}

static void codegen_stmt(CodegenState *state, ASTStmt *ast) {
    unsigned int saved = state->next_reg;

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

    // A declaration's slot outlives the statement: it belongs to the enclosing
    // block, which reclaims it at the closing brace.
    if (ast->kind != STMT_VAR_DECL) {
        codegen_release_registers(state, saved);
    }
}

static void codegen_return_stmt(CodegenState *state, ASTReturnStmt *ast) {
    unsigned int reg = codegen_expr(state, ast->result);
    unsigned int slots = ast->result ? type_slot_count(ast->result->type) : 1;

    // The slot count travels in the r2 field, so a wider return value cannot
    // be encoded at all.
    if (slots > VM_MAX_RETURN_SLOTS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, ast->result->span,
                       "struct is too large to return by value");
        }

        state->failed = true;
        return;
    }

    if (slots == 1) {
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN_N, 0, reg, slots));
}

static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast) {
    Span span = ast->initializer ? ast->initializer->span : (Span){0};

    ast->symbol->offset = codegen_alloc_slots(state, type_slot_count(ast->symbol->var.type),
                                              type_align_slots(ast->symbol->var.type), span);

    if (!ast->initializer) {
        return;
    }

    // The variable's own slot is already reserved; everything the initializer
    // allocates above it is a temporary and is reclaimed here.
    unsigned int saved = state->next_reg;

    unsigned int r1 = codegen_expr(state, ast->initializer);

    codegen_copy_slots(state, ast->symbol->offset, r1, type_slot_count(ast->symbol->var.type));

    codegen_release_registers(state, saved);
}

static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast) {
    // A field target is written in place through its base slot, so the target
    // is never materialised as a value first.
    if (ast->target->kind == EXPR_FIELD) {
        codegen_store_field(state, ast->target, codegen_expr(state, ast->value));
        return;
    }

    // Likewise a deref: '*p = v' writes through p rather than over it.
    if (ast->target->kind == EXPR_DEREF) {
        codegen_store_deref(state, ast->target, codegen_expr(state, ast->value));
        return;
    }

    unsigned int rd = codegen_expr(state, ast->target);
    unsigned int r1 = codegen_expr(state, ast->value);

    codegen_copy_slots(state, rd, r1, type_slot_count(ast->target->type));
}

static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast) {
    unsigned int saved = state->next_reg;

    for (int i = 0; i < ast->list.size; i++) {
        codegen_stmt(state, ast->list.data[i]);
    }

    codegen_release_registers(state, saved);
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

    // A multi-slot parameter owns consecutive slots starting at its offset, so
    // the callee addresses it by its base slot exactly like a local.
    for (int i = 0; i < ast->params.size; i++) {
        Symbol *param = ast->params.data[i]->symbol;

        param->offset = func_next_reg;
        func_next_reg += type_slot_count(param->var.type);
    }

    CodegenState func_state = (CodegenState){
        .chunk = func_chunk,
        .next_reg = func_next_reg,
        .max_reg = func_next_reg,
        .global_funcs = state->global_funcs,
        .diagnostics = state->diagnostics,
        .failed = false,
    };

    codegen_stmt(&func_state, ast->body);

    state->failed = state->failed || func_state.failed;

    OpCode last = func_chunk->instructions.size > 0
                      ? VM_DECODE_OPCODE(instruction_list_back(&func_chunk->instructions))
                      : OP_LOAD_CONST;

    if (func_chunk->instructions.size == 0 || (last != OP_RETURN && last != OP_RETURN_N)) {
        chunk_add_instruction(func_chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));
    }

    func_proto_list_emplace(state->global_funcs, proto_index,
                            (FuncPrototype){
                                .chunk = func_chunk,
                                .arity = ast->params.size,
                                .max_registers = func_state.max_reg,
                            });
}

static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast) {
    switch (ast->kind) {
    case EXPR_LITERAL:
        return codegen_literal_expr(state, ast);
    case EXPR_VARIABLE:
        return codegen_variable_expr(ast);
    case EXPR_BIN_OP:
        return codegen_bin_op_expr(state, ast);
    case EXPR_CALL:
        return codegen_call_expr(state, ast);
    case EXPR_FIELD:
        return codegen_field_expr(state, ast);
    case EXPR_ADDR_OF:
        return codegen_addr_of_expr(state, ast);
    case EXPR_DEREF:
        return codegen_deref_expr(state, ast);
    }

    assert(0 && "unknown expression kind");
    abort();
}

static Value value_from_literal(Literal lit) {
    switch (lit.kind) {
    case TYPE_INT:
        return (Value){.as_int = lit.as_int};
    case TYPE_FLOAT:
        return (Value){.as_float = lit.as_float};
    case TYPE_BOOL:
        return (Value){.as_int = lit.as_int};
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
    unsigned int arg_slots = 0;
    for (size_t i = 0; i < arg_count; i++) {
        arg_slots += type_slot_count(node->call.args.data[i]->type);
    }

    unsigned int return_slots = type_slot_count(node->type);

    // The callee's frame is based at dest, so its parameters overlap the
    // argument block. A struct return larger than that block still needs room
    // at dest, hence the max rather than just the arguments.
    unsigned int reserved = 1 + arg_slots;
    if (return_slots > reserved) {
        reserved = return_slots;
    }

    // dest and the argument slots must be contiguous, so the whole block is
    // reserved before evaluating anything.
    unsigned int dest = codegen_alloc_slots(state, reserved, 1, node->span);

    // Only the result slots outlive the call; everything above them is the
    // argument block and is released once the call is emitted.
    unsigned int saved = dest + return_slots;

    unsigned int offset = 1;
    for (size_t i = 0; i < arg_count; i++) {
        ASTExpr *arg = node->call.args.data[i];
        unsigned int slots = type_slot_count(arg->type);

        unsigned int arg_reg = codegen_expr(state, arg);

        codegen_copy_slots(state, dest + offset, arg_reg, slots);
        offset += slots;
    }

    // The prototype index rides in an 8-bit field. Masking alone would encode
    // a call to the wrong function, so it is rejected rather than truncated.
    if (node->symbol->offset > VM_MAX_REGISTERS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "too many functions in one script");
        }

        state->failed = true;
        return dest;
    }

    // argc counts slots, not arguments: that is what sizing the callee's frame
    // needs. The resolver has already checked the argument count.
    Instruction call = VM_ENCODE_R(OP_CALL, dest, node->symbol->offset, arg_slots);
    chunk_add_instruction(state->chunk, call);

    codegen_release_registers(state, saved);

    return dest;
}

typedef struct FieldTarget FieldTarget;
static void codegen_store_indirect(CodegenState *state, ASTExpr *node, FieldTarget target, unsigned int src,
                                   unsigned int slots);

// A resolved field chain: a base slot plus a byte offset within it. When
// 'indirect' is set the base slot pair holds an address rather than the struct
// itself, which is what a deref in the chain produces.
struct FieldTarget {
    unsigned int base;
    size_t offset;
    bool indirect;
};

// Walks a field chain down to whatever the outermost struct lives in,
// accumulating the byte offsets on the way. Nested structs are inline, so
// 'outer.inner.x' is one base plus a single summed offset; a deref stops the
// walk, because from there on the base is an address computed at runtime.
//
// 'auto_deref' says whether a pointer at the bottom of the chain is reached
// through or addressed. Field access reaches through it — that is 'p.health'
// where p is a '*Player' — but '&p' wants the address of p itself.
static FieldTarget codegen_field_base(CodegenState *state, ASTExpr *node, bool auto_deref) {
    if (node->kind == EXPR_FIELD) {
        // Everything below a field access is a struct being reached into, so a
        // pointer there is always reached through.
        FieldTarget target = codegen_field_base(state, node->field.target, true);
        target.offset += node->field.field->offset;
        return target;
    }

    if (node->kind == EXPR_DEREF) {
        return (FieldTarget){
            .base = codegen_expr(state, node->unary.target),
            .offset = 0,
            .indirect = true,
        };
    }

    return (FieldTarget){
        .base = codegen_expr(state, node),
        .offset = 0,
        .indirect = auto_deref && type_is_pointer(node->type),
    };
}

// An offset rides in an 8-bit operand, and only 1, 2 and 4 byte fields have an
// opcode. Both are compile-time facts, so a violation is reported once here.
static bool codegen_field_access_fits(CodegenState *state, ASTExpr *node, bool ok, size_t offset) {
    if (ok && offset <= VM_MAX_REGISTERS) {
        return true;
    }

    if (!state->failed) {
        diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "struct is too large for a frame");
    }

    state->failed = true;
    return false;
}

// The slots a struct occupies, addressed directly. Only valid for a direct
// target: through a pointer there is no slot to name.
static unsigned int codegen_field_slots(FieldTarget target) {
    assert(!target.indirect && "an indirect struct field has no slot of its own");
    assert(target.offset % sizeof(Value) == 0 && "a struct field is always slot-aligned");

    return target.base + (unsigned int)(target.offset / sizeof(Value));
}

// Copies a struct out of the address a pointer holds into fresh slots, so the
// result reads like any other struct-valued expression.
static unsigned int codegen_load_indirect_struct(CodegenState *state, ASTExpr *node, FieldTarget target,
                                                 unsigned int slots) {
    if (target.offset > VM_MAX_REGISTERS || slots > VM_MAX_REGISTERS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "struct is too large for a frame");
        }

        state->failed = true;
        return 0;
    }

    unsigned int rd = codegen_alloc_slots(state, slots, type_align_slots(node->type), node->span);

    // The offset is folded into the address first, so OP_LOAD_PTR_N needs only
    // a base and a count.
    unsigned int address = target.base;

    if (target.offset > 0) {
        address = codegen_alloc_slots(state, VM_POINTER_SLOTS, VM_POINTER_SLOTS, node->span);
        chunk_add_instruction(state->chunk,
                              VM_ENCODE_R(OP_ADD_PTR, address, target.base, (unsigned int)target.offset));
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_LOAD_PTR_N, rd, address, slots));

    return rd;
}

static unsigned int codegen_field_expr(CodegenState *state, ASTExpr *node) {
    FieldTarget target = codegen_field_base(state, node, true);

    // A struct-typed field is addressed, not loaded: its slots are already laid
    // out inline, so the caller reads them where they sit. Through a pointer
    // there are no such slots, so it is copied out instead.
    if (type_is_struct(node->type)) {
        if (target.indirect) {
            return codegen_load_indirect_struct(state, node, target, type_slot_count(node->type));
        }

        return codegen_field_slots(target);
    }

    bool ok;
    OpCode op = field_opcode_for(node->field.field->type->size, true, target.indirect, &ok);

    if (!codegen_field_access_fits(state, node, ok, target.offset)) {
        return 0;
    }

    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, (unsigned int)target.offset));

    return rd;
}

static void codegen_store_field(CodegenState *state, ASTExpr *node, unsigned int src) {
    FieldTarget target = codegen_field_base(state, node, true);

    if (type_is_struct(node->type)) {
        if (target.indirect) {
            codegen_store_indirect(state, node, target, src, type_slot_count(node->type));
            return;
        }

        codegen_copy_slots(state, codegen_field_slots(target), src, type_slot_count(node->type));
        return;
    }

    bool ok;
    OpCode op = field_opcode_for(node->field.field->type->size, false, target.indirect, &ok);

    if (!codegen_field_access_fits(state, node, ok, target.offset)) {
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, target.base, src, (unsigned int)target.offset));
}

// Writes a run of slots to the address a pointer holds, folding any field
// offset into the address first.
static void codegen_store_indirect(CodegenState *state, ASTExpr *node, FieldTarget target, unsigned int src,
                                   unsigned int slots) {
    if (target.offset > VM_MAX_REGISTERS || slots > VM_MAX_REGISTERS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "struct is too large for a frame");
        }

        state->failed = true;
        return;
    }

    unsigned int address = target.base;

    if (target.offset > 0) {
        address = codegen_alloc_slots(state, VM_POINTER_SLOTS, VM_POINTER_SLOTS, node->span);
        chunk_add_instruction(state->chunk,
                              VM_ENCODE_R(OP_ADD_PTR, address, target.base, (unsigned int)target.offset));
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_STORE_PTR_N, address, src, slots));
}

// '&x' materialises the address of whatever slots the target occupies. The
// target is addressable by construction: the resolver rejected anything else.
static unsigned int codegen_addr_of_expr(CodegenState *state, ASTExpr *node) {
    ASTExpr *inner = node->unary.target;

    // '&*p' is just p: the deref would only load what the address already is.
    if (inner->kind == EXPR_DEREF) {
        return codegen_expr(state, inner->unary.target);
    }

    // '&p' where p is a pointer names p itself, so the chain must stop at it
    // rather than reach through.
    FieldTarget target = codegen_field_base(state, inner, false);

    unsigned int rd = codegen_alloc_slots(state, VM_POINTER_SLOTS, VM_POINTER_SLOTS, node->span);

    if (target.offset > VM_MAX_REGISTERS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "struct is too large for a frame");
        }

        state->failed = true;
        return rd;
    }

    // Through a pointer the base is already an address, so the field offset is
    // added to it rather than to a slot index.
    OpCode op = target.indirect ? OP_ADD_PTR : OP_ADDR_OF;

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, (unsigned int)target.offset));

    return rd;
}

// '*p' reads what p points at: a whole run of slots for a struct, a single
// value otherwise.
static unsigned int codegen_deref_expr(CodegenState *state, ASTExpr *node) {
    FieldTarget target = {
        .base = codegen_expr(state, node->unary.target),
        .offset = 0,
        .indirect = true,
    };

    unsigned int slots = type_slot_count(node->type);

    if (type_is_struct(node->type) || slots > 1) {
        return codegen_load_indirect_struct(state, node, target, slots);
    }

    bool ok;
    OpCode op = field_opcode_for(node->type->size, true, true, &ok);

    if (!codegen_field_access_fits(state, node, ok, 0)) {
        return 0;
    }

    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, 0));

    return rd;
}

// Assignment through a deref: '*p = v' writes into whatever p points at
// instead of overwriting p.
static void codegen_store_deref(CodegenState *state, ASTExpr *node, unsigned int src) {
    FieldTarget target = {
        .base = codegen_expr(state, node->unary.target),
        .offset = 0,
        .indirect = true,
    };

    unsigned int slots = type_slot_count(node->type);

    if (type_is_struct(node->type) || slots > 1) {
        codegen_store_indirect(state, node, target, src, slots);
        return;
    }

    bool ok;
    OpCode op = field_opcode_for(node->type->size, false, true, &ok);

    if (!codegen_field_access_fits(state, node, ok, 0)) {
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, target.base, src, 0));
}

static void codegen_copy_slots(CodegenState *state, unsigned int dest, unsigned int src, unsigned int count) {
    if (dest == src) {
        return;
    }

    for (unsigned int i = 0; i < count; i++) {
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, dest + i, src + i, 0));
    }
}

// Every variable is a frame local now, including a top-level one: it lives in
// frame zero. The symbol already names its slot, so a read is free.
static unsigned int codegen_variable_expr(ASTExpr *node) { return node->symbol->offset; }

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
    return codegen_alloc_slots(state, 1, 1, span);
}

// Returns the first of count consecutive slots, aligned to align_slots. An
// 8-byte value needs an even slot index to land on an 8-byte boundary, which
// costs at most one wasted slot.
static unsigned int codegen_alloc_slots(CodegenState *state, unsigned int count, unsigned int align_slots,
                                        Span span) {
    if (align_slots > 1 && state->next_reg % align_slots != 0) {
        state->next_reg += align_slots - state->next_reg % align_slots;
    }

    if (count > VM_MAX_REGISTERS || state->next_reg > VM_MAX_REGISTERS - count) {
        if (!state->failed) {
            // A single value that cannot fit a frame has a different cause and
            // a different fix than a function that simply grew too big.
            if (count > 1) {
                diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "struct is too large for a frame");
            } else {
                diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "expression too complex");
            }
        }

        state->failed = true;
        return 0;
    }

    unsigned int reg = state->next_reg;
    state->next_reg += count;

    if (state->next_reg > state->max_reg) {
        state->max_reg = state->next_reg;
    }

    return reg;
}

// The single reclamation point. A variable whose address was taken is pinned
// for its enclosing block, and that already holds: a declaration's slot is
// skipped by its own statement's release and reclaimed only at the closing
// brace, which is exactly the pinned lifetime. Any future release that is
// finer-grained than a block has to consult Symbol.pinned here.
static void codegen_release_registers(CodegenState *state, unsigned int saved) { state->next_reg = saved; }

CodegenLabel codegen_create_label(CodegenState *state) {
    return (CodegenLabel){.position = chunk_add_instruction(state->chunk, 0)};
}

void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg) {
    Instruction patch = VM_ENCODE_I(op, reg, state->chunk->instructions.size - label.position - 1);
    chunk_patch_instruction(state->chunk, label.position, patch);
}
