#include "codegen.h"

#include "ast/ast.h"
#include "ast/expr.h"
#include "ast/stmt.h"
#include "scope.h"
#include "type/type.h"
#include "type/type_layout.h"
#include "vm/chunk.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include <assert.h>

#define slot_map_hash(key) (size_t)key
#define slot_map_key_equals(key, other) key == other
#define slot_map_key_dup(key) key
#define slot_map_entry_free(key, value)

typedef struct {
    unsigned int slot;
    unsigned int depth;
} SlotBinding;

GAB_HASH_MAP(SlotMap, slot_map, Binding *, SlotBinding)

#define SLOT_MAP_INITIAL_CAPACITY 16

#define proto_map_hash(key) (size_t)key
#define proto_map_key_equals(key, other) key == other
#define proto_map_key_dup(key) key
#define proto_map_entry_free(key, value)

GAB_HASH_MAP(ProtoMap, proto_map, Function *, size_t)

typedef struct {
    unsigned int slot;
    unsigned int depth;

    const Type *type;
} OwnedSlot;

#define owned_list_item_free(item) ((void)(item))
GAB_LIST(OwnedList, owned_list, OwnedSlot)

typedef struct {
    size_t position;
    unsigned int cond_reg;
} CodegenLabel;

#define codegen_label_list_item_free(item) ((void)(item))
GAB_LIST(CodegenLabelList, codegen_label_list, CodegenLabel)

typedef struct LoopContext {
    CodegenLabelList breaks;
    CodegenLabelList continues;

    unsigned int depth;
} LoopContext;

typedef struct {
    Chunk *chunk;
    unsigned int next_reg;

    unsigned int max_reg;

    Unit *unit;
    Arena *arena;

    TypeRegistry *registry;

    StringPool *strings;

    ProtoMap *local_protos;

    SlotMap *slots;

    OwnedList owned;

    OwnedList temporaries;

    FrameRefList frame_refs;

    unsigned int depth;

    LoopContext *loop;

    Diagnostics *diagnostics;
    bool failed;
} CodegenState;

typedef struct {
    unsigned int base;
    size_t offset;
    bool indirect;
} FieldTarget;

typedef enum {
    RHS_REGISTER,

    RHS_IMMEDIATE,

    RHS_CONSTANT,
} RhsKind;

static void codegen_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_return_stmt(CodegenState *state, ASTReturnStmt *ast);
static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast);
static bool codegen_expr_into(CodegenState *state, ASTExpr *value, unsigned int dest);
static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast);
static void codegen_compound_assign_stmt(CodegenState *state, ASTCompoundAssignStmt *ast);
static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast);
static bool stmt_may_assign(const ASTStmt *stmt, const Binding *binding);
static bool for_is_countable(const ASTForStmt *ast, const Binding **counter, const Binding **bound);
static void codegen_for_stmt(CodegenState *state, ASTForStmt *ast);
static void codegen_jump_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_if_stmt(CodegenState *state, ASTIfStmt *ast);
static void codegen_reserve_proto(CodegenState *state, ASTFuncDecl *ast);
static void codegen_func_decl_stmt(CodegenState *state, ASTStmt *stmt);

static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast);
static Constant value_from_literal(Literal lit);
static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_variable_expr(CodegenState *state, ASTExpr *node);
static void codegen_emit_call(CodegenState *state, unsigned int dest, Function *callee, Span span);
static unsigned int codegen_call_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_field_expr(CodegenState *state, ASTExpr *node);
static FieldTarget codegen_index_target(CodegenState *state, ASTExpr *node);
static unsigned int codegen_addr_of_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_deref_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_neg_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_not_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_cast_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_new_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_index_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_array_lit_expr(CodegenState *state, ASTExpr *node);

static FieldTarget codegen_resolve_field_target(CodegenState *state, ASTExpr *node, bool auto_deref);
static bool codegen_field_access_fits(CodegenState *state, ASTExpr *node, bool ok, size_t offset);
static unsigned int field_target_slot_count(FieldTarget target);
static unsigned int codegen_load_indirect_struct(CodegenState *state, ASTExpr *node, const Type *type,
                                                 FieldTarget target, unsigned int slots);
static void codegen_store_indirect(CodegenState *state, ASTExpr *node, FieldTarget target, unsigned int src,
                                   unsigned int slots);
static void codegen_store_field(CodegenState *state, ASTExpr *node, unsigned int src);
static void codegen_store_deref(CodegenState *state, ASTExpr *node, unsigned int src);
static void codegen_store_index(CodegenState *state, ASTExpr *node, unsigned int src);
static void codegen_addr_of_into(CodegenState *state, ASTExpr *inner, unsigned int rd, Span span);

static bool expr_is_immediate_operand(const ASTExpr *node, unsigned int *out);
static unsigned int codegen_rhs(CodegenState *state, BinOp op, ASTExpr *rhs, const Type *left_type,
                                RhsKind *kind);
static OpCode bin_op_opcode_for(BinOp op, const Type *left_type, RhsKind kind);
static OpCode bin_op_to_float_op(BinOp bin_op);
static OpCode bin_op_to_int_op(BinOp bin_op);
static void codegen_emit_bin_op(CodegenState *state, ASTExpr *node, unsigned int dest, unsigned int lhs,
                                unsigned int rhs, RhsKind kind);
static unsigned int codegen_bin_op_into(CodegenState *state, ASTExpr *node, unsigned int dest);
static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_logical_expr(CodegenState *state, ASTExpr *node);

static CodegenLabel codegen_create_label(CodegenState *state);
static void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg);
static void codegen_emit_loop(CodegenState *state, size_t target);

static bool expr_yields_owned(TypeRegistry *registry, const ASTExpr *expr);

static void codegen_own_slot(CodegenState *state, unsigned int slot, const Type *type);
static void codegen_own_slot_at(CodegenState *state, unsigned int slot, const Type *type, unsigned int depth);
static void codegen_record_frame_ref(CodegenState *state, unsigned int slot, const Type *type);

static bool codegen_slot_is_owned(const CodegenState *state, unsigned int slot);

static void codegen_release_owned(CodegenState *state, unsigned int keep_depth, unsigned int moved);

static void codegen_disown_slot(CodegenState *state, unsigned int slot);

static void codegen_drop_temporary(CodegenState *state, unsigned int slot);

static void codegen_emit_releases_below(CodegenState *state, unsigned int keep_depth);

static void codegen_emit_release(CodegenState *state, unsigned int slot, const Type *type);

static unsigned int codegen_slot_of(CodegenState *state, Binding *binding);
static unsigned int codegen_decl_depth_of(CodegenState *state, Binding *binding);
static void codegen_set_slot(CodegenState *state, Binding *binding, unsigned int slot);
static unsigned int codegen_alloc_register(CodegenState *state, Span span);
static unsigned int codegen_alloc_slots(CodegenState *state, unsigned int count, unsigned int align_slots,
                                        Span span);
static void codegen_release_registers(CodegenState *state, unsigned int saved);
static void codegen_copy_slots(CodegenState *state, unsigned int dest, unsigned int src, unsigned int count);
static unsigned int codegen_lend_expr(CodegenState *state, ASTExpr *node);

static size_t slot_release_width(TypeRegistry *registry, const Type *type);
static unsigned int type_slot_count(TypeRegistry *registry, const Type *type);
static unsigned int type_align_slots(TypeRegistry *registry, const Type *type);
static bool type_is_struct(const Type *type);

static bool type_owns_through_members(TypeRegistry *registry, const Type *type);

static bool type_moves_as_slots(TypeRegistry *registry, const Type *type);
static OpCode field_opcode_for(size_t size, bool load, bool indirect, bool *ok);

Unit *codegen_generate(ASTUnit *ast, Arena *arena, StringPool *strings, TypeRegistry *registry,
                       Diagnostics *diagnostics) {
    Unit *unit = calloc(1, sizeof(Unit));

    if (!unit) {
        return NULL;
    }

    unit->prototypes = func_proto_list_create();
    unit->extern_protos = extern_proto_list_create();
    unit->types = type_list_create();
    unit->type_shapes = heap_shape_list_create();
    unit->strings = string_list_create();
    unit->proto_relocations = relocation_list_create();
    unit->extern_relocations = relocation_list_create();
    unit->type_relocations = relocation_list_create();
    unit->string_relocations = relocation_list_create();
    unit->bindings = proto_binding_list_create();
    unit->externs = extern_request_list_create();
    unit->arena = arena;

    CodegenState state = {
        .chunk = chunk_create(),
        .next_reg = 0,
        .max_reg = 0,
        .unit = unit,
        .arena = arena,
        .registry = registry,
        .strings = strings,
        .local_protos = proto_map_create(SLOT_MAP_INITIAL_CAPACITY),
        .slots = slot_map_create(SLOT_MAP_INITIAL_CAPACITY),
        .owned = owned_list_create(),
        .temporaries = owned_list_create(),
        .depth = 0,
        .frame_refs = frame_ref_list_create(),
        .diagnostics = diagnostics,
        .failed = false,
    };

    for (size_t i = 0; i < ast->statements.size; i++) {
        ASTStmt *stmt = ast->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL) {
            codegen_reserve_proto(&state, &stmt->func_decl);
        }
    }

    for (size_t i = 0; i < ast->statements.size; i++) {
        codegen_stmt(&state, ast->statements.data[i]);
    }

    OpCode last = state.chunk->instructions.size > 0
                      ? VM_DECODE_OPCODE(instruction_list_back(&state.chunk->instructions))
                      : OP_LOAD_CONST;

    if (state.chunk->instructions.size == 0 || (last != OP_RETURN && last != OP_RETURN_N)) {
        chunk_add_instruction(state.chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));
    }

    slot_map_destroy(state.slots);
    owned_list_free(&state.owned);
    owned_list_free(&state.temporaries);
    proto_map_destroy(state.local_protos);

    if (state.failed) {
        chunk_free(state.chunk);
        frame_ref_list_free(&state.frame_refs);
        unit_free(unit);
        return NULL;
    }

    unit->top_level.chunk = state.chunk;
    unit->top_level.max_registers = (int)state.max_reg;
    unit->top_level.refs = state.frame_refs;

    return unit;
}

static void codegen_drop_temporary(CodegenState *state, unsigned int slot) {
    for (size_t i = 0; i < state->temporaries.size; i++) {
        if (state->temporaries.data[i].slot != slot) {
            continue;
        }

        state->temporaries.data[i] = state->temporaries.data[state->temporaries.size - 1];
        state->temporaries.size--;
        return;
    }
}

static void codegen_release_temporaries(CodegenState *state) {
    while (state->temporaries.size > 0) {
        OwnedSlot temporary = state->temporaries.data[--state->temporaries.size];

        codegen_emit_release(state, temporary.slot, temporary.type);
    }
}

static void codegen_stmt(CodegenState *state, ASTStmt *ast) {
    unsigned int saved = state->next_reg;

    switch (ast->kind) {
    case STMT_EXPR: {
        unsigned int reg = codegen_expr(state, ast->expr.value);

        if (expr_yields_owned(state->registry, ast->expr.value)) {
            codegen_emit_release(state, reg, ast->expr.value->type);
        }
        break;
    }
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
    case STMT_COMPOUND_ASSIGN: {
        codegen_compound_assign_stmt(state, &ast->compound_assign);
        break;
    }
    case STMT_BLOCK: {
        codegen_block_stmt(state, &ast->block);
        break;
    }
    case STMT_FOR: {
        codegen_for_stmt(state, &ast->forstmt);
        break;
    }
    case STMT_JUMP: {
        codegen_jump_stmt(state, ast);
        break;
    }
    case STMT_IF: {
        codegen_if_stmt(state, &ast->ifstmt);
        break;
    }
    case STMT_FUNC_DECL:
        codegen_func_decl_stmt(state, ast);
        break;
    case STMT_STRUCT_DECL:

        break;
    }

    codegen_release_temporaries(state);

    if (ast->kind != STMT_VAR_DECL) {
        codegen_release_registers(state, saved);
    }
}

static void codegen_return_stmt(CodegenState *state, ASTReturnStmt *ast) {
    unsigned int reg = codegen_expr(state, ast->result);
    unsigned int slots = ast->result ? type_slot_count(state->registry, ast->result->type) : 1;

    if (slots > VM_MAX_RETURN_SLOTS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, ast->result->span,
                       "struct is too large to return by value");
        }

        state->failed = true;
        return;
    }

    codegen_drop_temporary(state, reg);

    codegen_release_temporaries(state);

    codegen_release_owned(state, 0, ast->result ? reg : VM_INVALID_REGISTER);

    if (slots == 1) {
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN_N, 0, reg, slots));
}

typedef enum {
    OWNING_SLOT_NULL,

    OWNING_SLOT_OWN,

    OWNING_SLOT_DISOWN,
} OwningSlotAction;

static void codegen_walk_owning_slots(CodegenState *state, const Type *type, unsigned int base,
                                      OwningSlotAction action) {
    if (type_owns_through_an_address(type)) {
        switch (action) {
        case OWNING_SLOT_NULL:
            chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_NULL, base, 0));
            break;
        case OWNING_SLOT_OWN:
            codegen_own_slot(state, base, type);
            break;
        case OWNING_SLOT_DISOWN:
            codegen_disown_slot(state, base);
            break;
        }

        return;
    }

    if (type && type_kind(type) == TYPE_ARRAY) {
        if (!type_registry_owns(state->registry, type)) {
            return;
        }

        switch (action) {
        case OWNING_SLOT_NULL:
            chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_NULL, base, 0));
            break;
        case OWNING_SLOT_OWN:
            codegen_own_slot(state, base, type);
            break;
        case OWNING_SLOT_DISOWN:
            codegen_disown_slot(state, base);
            break;
        }

        return;
    }

    const TypeFields *fields = type_registry_fields_of(state->registry, type);

    if (!type || fields->count == 0) {
        return;
    }

    const TypeLayout *layout = type_registry_layout_of(state->registry, type);

    for (size_t i = 0; i < fields->count; i++) {
        codegen_walk_owning_slots(state, fields->fields[i].type,
                                  base + (unsigned int)(layout->offsets[i] / VM_SLOT_SIZE), action);
    }
}
static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast) {
    Span span = ast->initializer ? ast->initializer->span : (Span){0};

    codegen_set_slot(state, ast->binding,
                     codegen_alloc_slots(state, type_slot_count(state->registry, ast->binding->var.type),
                                         type_align_slots(state->registry, ast->binding->var.type), span));

    bool is_ref = ast->binding->var.type && type_kind(ast->binding->var.type) == TYPE_REF;

    if (!ast->initializer) {
        if (!is_ref) {
            unsigned int slot = codegen_slot_of(state, ast->binding);

            codegen_walk_owning_slots(state, ast->binding->var.type, slot, OWNING_SLOT_NULL);

            if (type_owns_through_members(state->registry, ast->binding->var.type)) {
                codegen_walk_owning_slots(state, ast->binding->var.type, slot, OWNING_SLOT_OWN);
            }
        }

        return;
    }

    unsigned int saved = state->next_reg;

    unsigned int slot = codegen_slot_of(state, ast->binding);

    if (!is_ref && !type_registry_owns(state->registry, ast->binding->var.type) &&
        codegen_expr_into(state, ast->initializer, slot)) {
        codegen_release_registers(state, saved);
        return;
    }

    unsigned int r1 = codegen_expr(state, ast->initializer);

    codegen_copy_slots(state, slot, r1, type_slot_count(state->registry, ast->binding->var.type));

    if (!is_ref && expr_yields_owned(state->registry, ast->initializer)) {
        codegen_walk_owning_slots(state, ast->binding->var.type, slot, OWNING_SLOT_OWN);

        codegen_drop_temporary(state, r1);
    }

    codegen_release_registers(state, saved);
}

static bool codegen_expr_into(CodegenState *state, ASTExpr *value, unsigned int dest) {
    if (type_slot_count(state->registry, value->type) != 1 ||
        type_registry_owns(state->registry, value->type)) {
        return false;
    }

    switch (value->kind) {
    case EXPR_LITERAL: {
        unsigned int index = constpool_add(state->chunk->const_pool, value_from_literal(value->lit));

        chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_LOAD_CONST, dest, index));
        return true;
    }
    case EXPR_VARIABLE:
        codegen_copy_slots(state, dest, codegen_slot_of(state, value->binding), 1);
        return true;

    case EXPR_BIN_OP:
        if (value->bin_op.op == BIN_OP_AND || value->bin_op.op == BIN_OP_OR) {
            return false;
        }

        codegen_bin_op_into(state, value, dest);
        return true;
    default:
        return false;
    }
}

static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast) {
    bool target_owns = (ast->target->kind == EXPR_FIELD || ast->target->kind == EXPR_DEREF ||
                        ast->target->kind == EXPR_INDEX) &&
                       type_registry_owns(state->registry, ast->target->type);

    if (target_owns) {
        unsigned int held;

        switch (ast->target->kind) {
        case EXPR_FIELD:
            held = codegen_field_expr(state, ast->target);
            break;
        case EXPR_INDEX:
            held = codegen_index_expr(state, ast->target);
            break;
        default:
            held = codegen_deref_expr(state, ast->target);
            break;
        }

        unsigned int width = type_slot_count(state->registry, ast->target->type);
        unsigned int old = codegen_alloc_slots(
            state, width, type_align_slots(state->registry, ast->target->type), ast->target->span);

        codegen_copy_slots(state, old, held, width);

        unsigned int src = codegen_expr(state, ast->value);

        switch (ast->target->kind) {
        case EXPR_FIELD:
            codegen_store_field(state, ast->target, src);
            break;
        case EXPR_INDEX:
            codegen_store_index(state, ast->target, src);
            break;
        default:
            codegen_store_deref(state, ast->target, src);
            break;
        }

        codegen_drop_temporary(state, src);

        codegen_emit_release(state, old, ast->target->type);
        return;
    }

    if (ast->target->kind == EXPR_FIELD) {
        codegen_store_field(state, ast->target, codegen_expr(state, ast->value));
        return;
    }

    if (ast->target->kind == EXPR_INDEX) {
        codegen_store_index(state, ast->target, codegen_expr(state, ast->value));
        return;
    }

    if (ast->target->kind == EXPR_DEREF) {
        codegen_store_deref(state, ast->target, codegen_expr(state, ast->value));
        return;
    }

    unsigned int rd = codegen_expr(state, ast->target);

    if (!type_registry_owns(state->registry, ast->target->type) && codegen_expr_into(state, ast->value, rd)) {
        return;
    }

    unsigned int r1 = codegen_expr(state, ast->value);

    bool target_is_ref = ast->target->type && type_kind(ast->target->type) == TYPE_REF;

    if (!target_is_ref && type_registry_owns(state->registry, ast->target->type) &&
        codegen_slot_is_owned(state, rd)) {
        unsigned int width = type_slot_count(state->registry, ast->target->type);
        unsigned int old = codegen_alloc_slots(
            state, width, type_align_slots(state->registry, ast->target->type), ast->target->span);

        codegen_copy_slots(state, old, rd, width);
        codegen_copy_slots(state, rd, r1, width);

        codegen_drop_temporary(state, r1);

        codegen_emit_release(state, old, ast->target->type);
        return;
    }

    codegen_copy_slots(state, rd, r1, type_slot_count(state->registry, ast->target->type));

    if (!target_is_ref && type_registry_owns(state->registry, ast->target->type) &&
        expr_yields_owned(state->registry, ast->value)) {
        Binding *target = ast_root_local(ast->target);

        codegen_own_slot_at(state, rd, ast->target->type,
                            target ? codegen_decl_depth_of(state, target) : state->depth);
    }
}

static void codegen_compound_assign_stmt(CodegenState *state, ASTCompoundAssignStmt *ast) {
    assert(type_slot_count(state->registry, ast->target->type) == 1 &&
           "a compound assignment target is a single slot");

    if (ast->target->kind == EXPR_VARIABLE) {
        unsigned int rd = codegen_expr(state, ast->target);

        RhsKind rhs_kind = RHS_REGISTER;
        unsigned int rhs = codegen_rhs(state, ast->op, ast->value, ast->target->type, &rhs_kind);

        OpCode op_code = bin_op_opcode_for(ast->op, ast->target->type, rhs_kind);

        chunk_add_instruction(state->chunk,
                              VM_ENCODE_RK(op_code, rd, rd, rhs, rhs_kind == RHS_IMMEDIATE ? 1 : 0));
        return;
    }

    FieldTarget target = ast->target->kind == EXPR_FIELD
                             ? codegen_resolve_field_target(state, ast->target, true)
                             : (FieldTarget){
                                   .base = codegen_expr(state, ast->target->unary.target),
                                   .offset = 0,
                                   .indirect = true,
                               };

    size_t size = type_registry_size_of(state->registry, ast->target->type);

    bool load_ok;
    OpCode load_op = field_opcode_for(size, true, target.indirect, &load_ok);

    if (!codegen_field_access_fits(state, ast->target, load_ok, target.offset)) {
        return;
    }

    unsigned int value = codegen_alloc_register(state, ast->target->span);
    chunk_add_instruction(state->chunk,
                          VM_ENCODE_R(load_op, value, target.base, (unsigned int)target.offset));

    RhsKind rhs_kind = RHS_REGISTER;
    unsigned int rhs = codegen_rhs(state, ast->op, ast->value, ast->target->type, &rhs_kind);

    OpCode op_code = bin_op_opcode_for(ast->op, ast->target->type, rhs_kind);

    chunk_add_instruction(state->chunk,
                          VM_ENCODE_RK(op_code, value, value, rhs, rhs_kind == RHS_IMMEDIATE ? 1 : 0));

    bool store_ok;
    OpCode store_op = field_opcode_for(size, false, target.indirect, &store_ok);

    if (!codegen_field_access_fits(state, ast->target, store_ok, target.offset)) {
        return;
    }

    chunk_add_instruction(state->chunk,
                          VM_ENCODE_R(store_op, target.base, value, (unsigned int)target.offset));
}

static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast) {
    unsigned int saved = state->next_reg;
    unsigned int enclosing_depth = state->depth++;

    for (size_t i = 0; i < ast->list.size; i++) {
        codegen_stmt(state, ast->list.data[i]);
    }

    codegen_release_owned(state, enclosing_depth, VM_INVALID_REGISTER);

    state->depth = enclosing_depth;
    codegen_release_registers(state, saved);
}

static bool stmt_may_assign(const ASTStmt *stmt, const Binding *binding) {
    if (!stmt) {
        return false;
    }

    switch (stmt->kind) {
    case STMT_ASSIGN:
        return stmt->assign.target->binding == binding;
    case STMT_COMPOUND_ASSIGN:
        return stmt->compound_assign.target->binding == binding;
    case STMT_VAR_DECL:
        return stmt->var_decl.binding == binding;
    case STMT_BLOCK:
        for (size_t i = 0; i < stmt->block.list.size; i++) {
            if (stmt_may_assign(stmt->block.list.data[i], binding)) {
                return true;
            }
        }

        return false;
    case STMT_IF:
        return stmt_may_assign(stmt->ifstmt.then_block, binding) ||
               stmt_may_assign(stmt->ifstmt.else_block, binding);
    case STMT_FOR:
        return stmt_may_assign(stmt->forstmt.init, binding) || stmt_may_assign(stmt->forstmt.post, binding) ||
               stmt_may_assign(stmt->forstmt.body, binding);
    case STMT_EXPR:
    case STMT_FUNC_DECL:
    case STMT_STRUCT_DECL:
    case STMT_JUMP:
    case STMT_RETURN:
        return false;
    }

    return true;
}

static bool is_one(const ASTExpr *expr) {
    return expr->kind == EXPR_LITERAL && expr->lit.kind == TYPE_INT && expr->lit.as_int == 1;
}

static bool for_step_is_one(const ASTStmt *post, const Binding *counter) {
    if (post->kind == STMT_COMPOUND_ASSIGN) {
        const ASTCompoundAssignStmt *step = &post->compound_assign;

        if (step->op != BIN_OP_ADD || step->target->kind != EXPR_VARIABLE ||
            step->target->binding != counter) {
            return false;
        }

        return is_one(step->value);
    }

    if (post->kind != STMT_ASSIGN) {
        return false;
    }

    const ASTExpr *target = post->assign.target;
    const ASTExpr *value = post->assign.value;

    if (target->kind != EXPR_VARIABLE || target->binding != counter) {
        return false;
    }

    if (value->kind != EXPR_BIN_OP || value->bin_op.op != BIN_OP_ADD) {
        return false;
    }

    const ASTExpr *lhs = value->bin_op.left;
    const ASTExpr *rhs = value->bin_op.right;

    if (lhs->kind == EXPR_VARIABLE && lhs->binding == counter) {
        return is_one(rhs);
    }

    if (rhs->kind == EXPR_VARIABLE && rhs->binding == counter) {
        return is_one(lhs);
    }

    return false;
}

static bool for_is_countable(const ASTForStmt *ast, const Binding **counter, const Binding **bound) {
    if (!ast->condition || !ast->post || !ast->body) {
        return false;
    }

    if (ast->condition->kind != EXPR_BIN_OP || ast->condition->bin_op.op != BIN_OP_LESS) {
        return false;
    }

    const ASTExpr *left = ast->condition->bin_op.left;
    const ASTExpr *right = ast->condition->bin_op.right;

    if (left->kind != EXPR_VARIABLE || right->kind != EXPR_VARIABLE) {
        return false;
    }

    if (!left->type || type_kind(left->type) != TYPE_INT || !right->type ||
        type_kind(right->type) != TYPE_INT) {
        return false;
    }

    if (!for_step_is_one(ast->post, left->binding)) {
        return false;
    }

    if (left->binding->pinned || right->binding->pinned) {
        return false;
    }

    if (stmt_may_assign(ast->body, left->binding) || stmt_may_assign(ast->body, right->binding)) {
        return false;
    }

    *counter = left->binding;
    *bound = right->binding;

    return true;
}

static void codegen_for_stmt(CodegenState *state, ASTForStmt *ast) {
    unsigned int saved = state->next_reg;
    unsigned int enclosing_depth = state->depth++;

    if (ast->init) {
        codegen_stmt(state, ast->init);
    }

    LoopContext *enclosing_loop = state->loop;
    LoopContext loop = {
        .breaks = codegen_label_list_create(),
        .continues = codegen_label_list_create(),

        .depth = state->depth,
    };
    state->loop = &loop;

    const Binding *counter = NULL;
    const Binding *bound = NULL;

    if (for_is_countable(ast, &counter, &bound)) {
        unsigned int counter_reg = codegen_slot_of(state, (Binding *)counter);
        unsigned int bound_reg = codegen_slot_of(state, (Binding *)bound);

        unsigned int left = codegen_alloc_register(state, ast->condition->span);

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_LOOP_INIT, left, counter_reg, bound_reg));

        unsigned int entry_saved = state->next_reg;
        unsigned int entry = codegen_alloc_register(state, ast->condition->span);

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_CMP_LTI, entry, counter_reg, bound_reg));

        CodegenLabel entry_label = codegen_create_label(state);
        codegen_release_registers(state, entry_saved);

        size_t body_start = state->chunk->instructions.size;

        codegen_stmt(state, ast->body);

        for (size_t i = 0; i < loop.continues.size; i++) {
            codegen_patch_jump(state, loop.continues.data[i], OP_JMP, 0);
        }

        ptrdiff_t back = (ptrdiff_t)body_start - (ptrdiff_t)(state->chunk->instructions.size + 1);

        if (back >= -VM_MAX_LOOP_OFFSET) {
            chunk_add_instruction(state->chunk,
                                  VM_ENCODE_R(OP_FOR_LOOP, counter_reg, left, (unsigned int)-back));

            codegen_patch_jump(state, entry_label, OP_JMP_IF_FALSE, entry);

            for (size_t i = 0; i < loop.breaks.size; i++) {
                codegen_patch_jump(state, loop.breaks.data[i], OP_JMP, 0);
            }

            state->loop = enclosing_loop;
            codegen_label_list_free(&loop.breaks);
            codegen_label_list_free(&loop.continues);

            codegen_release_owned(state, enclosing_depth, VM_INVALID_REGISTER);

            state->depth = enclosing_depth;
            codegen_release_registers(state, saved);
            return;
        }
    }

    size_t condition_target = state->chunk->instructions.size;

    CodegenLabel exit_label = {0};
    unsigned int exit_reg = 0;
    unsigned int condition_saved = state->next_reg;

    if (ast->condition) {
        exit_reg = codegen_expr(state, ast->condition);
        exit_label = codegen_create_label(state);

        codegen_release_registers(state, condition_saved);
    }

    codegen_stmt(state, ast->body);

    for (size_t i = 0; i < loop.continues.size; i++) {
        codegen_patch_jump(state, loop.continues.data[i], OP_JMP, 0);
    }

    if (ast->post) {
        codegen_stmt(state, ast->post);
    }

    codegen_emit_loop(state, condition_target);

    if (ast->condition) {
        codegen_patch_jump(state, exit_label, OP_JMP_IF_FALSE, exit_reg);
    }

    for (size_t i = 0; i < loop.breaks.size; i++) {
        codegen_patch_jump(state, loop.breaks.data[i], OP_JMP, 0);
    }

    state->loop = enclosing_loop;
    codegen_label_list_free(&loop.breaks);
    codegen_label_list_free(&loop.continues);

    codegen_release_owned(state, enclosing_depth, VM_INVALID_REGISTER);

    state->depth = enclosing_depth;
    codegen_release_registers(state, saved);
}

static void codegen_jump_stmt(CodegenState *state, ASTStmt *ast) {
    assert(state->loop && "'break' outside a loop reached codegen");

    codegen_emit_releases_below(state, state->loop->depth);

    CodegenLabel label = codegen_create_label(state);

    if (ast->jump.is_break) {
        codegen_label_list_add(&state->loop->breaks, label);
        return;
    }

    codegen_label_list_add(&state->loop->continues, label);
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

static size_t codegen_reserve_function(CodegenState *state, Function *function) {
    size_t local;

    if (function_runs_native(function)) {
        extern_proto_list_add(&state->unit->extern_protos, (ExternProto){0});

        local = state->unit->extern_protos.size - 1;
    } else {
        FuncPrototype *proto = arena_alloc(state->arena, sizeof(FuncPrototype));
        *proto = (FuncPrototype){0};

        func_proto_list_add(&state->unit->prototypes, proto);

        local = state->unit->prototypes.size - 1;
    }

    proto_map_insert(state->local_protos, function, local);
    proto_binding_list_add(&state->unit->bindings,
                           (ProtoBinding){.function = function, .local_index = local});

    return local;
}

static void codegen_reserve_proto(CodegenState *state, ASTFuncDecl *ast) {
    if (!ast->function || proto_map_lookup(state->local_protos, ast->function)) {
        return;
    }

    codegen_reserve_function(state, ast->function);
}

static const size_t *codegen_reserve_instantiated(CodegenState *state, Function *function) {
    size_t local = codegen_reserve_function(state, function);

    state->unit->extern_protos.data[local] =
        (ExternProto){.body = (GabExternFn)function->body, .function = function};

    return proto_map_lookup(state->local_protos, function);
}

static void codegen_func_decl_stmt(CodegenState *state, ASTStmt *stmt) {
    ASTFuncDecl *ast = &stmt->func_decl;

    codegen_reserve_proto(state, ast);

    if (!ast->function) {
        return;
    }

    const size_t *local = proto_map_lookup(state->local_protos, ast->function);

    if (!local) {
        return;
    }

    size_t func_index = *local;

    if (ast->function->body_kind == BODY_HOST) {
        state->unit->extern_protos.data[func_index] = (ExternProto){.function = ast->function};

        extern_request_list_add(
            &state->unit->externs,
            (ExternRequest){.local_index = func_index, .function = ast->function, .span = stmt->span});

        return;
    }

    Chunk *func_chunk = chunk_create();

    unsigned int func_next_reg = 1;

    CodegenState func_state = (CodegenState){
        .chunk = func_chunk,
        .unit = state->unit,
        .arena = state->arena,
        .registry = state->registry,
        .strings = state->strings,
        .local_protos = state->local_protos,
        .slots = slot_map_create(SLOT_MAP_INITIAL_CAPACITY),
        .owned = owned_list_create(),
        .temporaries = owned_list_create(),
        .depth = 0,
        .frame_refs = frame_ref_list_create(),
        .diagnostics = state->diagnostics,
        .failed = false,
    };

    for (size_t i = 0; i < ast->params.size; i++) {
        Binding *param = ast->params.data[i]->binding;

        codegen_set_slot(&func_state, param, func_next_reg);
        func_next_reg += type_slot_count(state->registry, param->var.type);
    }

    if (func_next_reg > VM_MAX_FRAME_SLOTS) {
        diag_error(state->diagnostics, GAB_ERR_CODEGEN, stmt->span,
                   "function signature is too large for a frame");

        state->failed = true;

        chunk_free(func_chunk);
        frame_ref_list_free(&func_state.frame_refs);
        owned_list_free(&func_state.owned);
        owned_list_free(&func_state.temporaries);
        slot_map_destroy(func_state.slots);

        return;
    }

    func_state.next_reg = func_next_reg;
    func_state.max_reg = func_next_reg;

    func_state.depth = 1;

    for (size_t i = 0; i < ast->params.size; i++) {
        Binding *param = ast->params.data[i]->binding;

        if (type_registry_owns(state->registry, param->var.type)) {
            codegen_own_slot(&func_state, codegen_slot_of(&func_state, param), param->var.type);
        }
    }

    func_state.depth = 0;

    codegen_stmt(&func_state, ast->body);

    state->failed = state->failed || func_state.failed;

    OpCode last = func_chunk->instructions.size > 0
                      ? VM_DECODE_OPCODE(instruction_list_back(&func_chunk->instructions))
                      : OP_LOAD_CONST;

    if (func_chunk->instructions.size == 0 || (last != OP_RETURN && last != OP_RETURN_N)) {
        chunk_add_instruction(func_chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));
    }

    *state->unit->prototypes.data[func_index] = (FuncPrototype){
        .chunk = func_chunk,
        .arity = ast->params.size,
        .max_registers = func_state.max_reg,
        .refs = func_state.frame_refs,
    };

    owned_list_free(&func_state.owned);
    owned_list_free(&func_state.temporaries);
    slot_map_destroy(func_state.slots);
}

static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast) {
    if (ast->moves && ast->kind == EXPR_VARIABLE && ast->binding) {
        unsigned int reg = codegen_variable_expr(state, ast);

        codegen_walk_owning_slots(state, ast->type, codegen_slot_of(state, ast->binding), OWNING_SLOT_DISOWN);

        return reg;
    }

    switch (ast->kind) {
    case EXPR_LITERAL:
        return codegen_literal_expr(state, ast);
    case EXPR_VARIABLE:
        return codegen_variable_expr(state, ast);
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
    case EXPR_LEND:
        return codegen_lend_expr(state, ast);
    case EXPR_NEG:
        return codegen_neg_expr(state, ast);
    case EXPR_NOT:
        return codegen_not_expr(state, ast);
    case EXPR_CAST:
        return codegen_cast_expr(state, ast);
    case EXPR_NEW:
        return codegen_new_expr(state, ast);
    case EXPR_INDEX:
        return codegen_index_expr(state, ast);
    case EXPR_ARRAY_LIT:
        return codegen_array_lit_expr(state, ast);
    }

    assert(0 && "unknown expression kind");
    abort();
}

static Constant value_from_literal(Literal lit) {
    switch (lit.kind) {
    case TYPE_INT:
        return (Constant){.as_int = lit.as_int};
    case TYPE_FLOAT:
        return (Constant){.as_float = lit.as_float};
    case TYPE_BOOL:
        return (Constant){.as_int = lit.as_int};
    default:
        break;
    }

    assert(0 && "unknown type");
    abort();
}

static unsigned int codegen_lend_expr(CodegenState *state, ASTExpr *node) {
    const Type *reference = node->type;

    unsigned int source = codegen_expr(state, node->lend.target);

    unsigned int rd = codegen_alloc_slots(state, type_slot_count(state->registry, reference),
                                          type_align_slots(state->registry, reference), node->span);

    assert(node->lend.part_count > 0 && "a lend handed over nothing");

    unsigned int at = 0;

    for (size_t i = 0; i < node->lend.part_count; i++) {
        const LentPart *part = &node->lend.parts[i];

        assert(part->offset % VM_SLOT_SIZE == 0 && "a lent part is always slot-aligned");

        unsigned int width = (unsigned int)((part->size + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);

        codegen_copy_slots(state, rd + at, source + (unsigned int)(part->offset / VM_SLOT_SIZE), width);

        at += width;
    }

    return rd;
}

static unsigned int codegen_string_literal(CodegenState *state, ASTExpr *node) {
    String *text = node->lit.as_string;

    unsigned int rd = codegen_alloc_slots(state, type_slot_count(state->registry, node->type),
                                          type_align_slots(state->registry, node->type), node->span);

    size_t index = state->unit->strings.size;

    for (size_t i = 0; i < state->unit->strings.size; i++) {
        if (state->unit->strings.data[i] == text) {
            index = i;
            break;
        }
    }

    if (index == state->unit->strings.size) {
        string_list_add(&state->unit->strings, text);
    }

    size_t offset = chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_LOAD_STR, rd, (unsigned int)index));

    relocation_list_add(&state->unit->string_relocations,
                        (Relocation){.chunk = state->chunk, .offset = offset});

    return rd;
}

static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node) {
    if (node->lit.kind == TYPE_STR) {
        return codegen_string_literal(state, node);
    }

    unsigned int const_index = constpool_add(state->chunk->const_pool, value_from_literal(node->lit));
    unsigned int r1 = codegen_alloc_register(state, node->span);
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, r1, const_index);

    chunk_add_instruction(state->chunk, load_const);

    return r1;
}

static unsigned int codegen_variable_expr(CodegenState *state, ASTExpr *node) {
    return codegen_slot_of(state, node->binding);
}

static void codegen_emit_call(CodegenState *state, unsigned int dest, Function *callee, Span span) {
    const size_t *local = proto_map_lookup(state->local_protos, callee);

    if (!local && callee->func_index == FUNCTION_NO_BODY && callee->body) {
        local = codegen_reserve_instantiated(state, callee);
    }

    size_t index = local ? *local : callee->func_index;

    if (index == FUNCTION_NO_BODY) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "call to a function with no body");
        }

        state->failed = true;
        return;
    }

    bool runs_native = function_runs_native(callee);

    if (!local && index > (runs_native ? VM_MAX_EXTERN_PROTOS : VM_MAX_PROTOTYPES)) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "too many functions in one program");
        }

        state->failed = true;
        return;
    }

    size_t offset = chunk_add_instruction(
        state->chunk, VM_ENCODE_I(runs_native ? OP_CALL_EXTERN : OP_CALL, dest, (unsigned int)index));

    if (local) {
        relocation_list_add(runs_native ? &state->unit->extern_relocations : &state->unit->proto_relocations,
                            (Relocation){.chunk = state->chunk, .offset = offset});
    }
}

static bool param_owns(TypeRegistry *registry, const Function *callee, size_t index) {
    if (!callee || index >= callee->param_count) {
        return false;
    }

    const Type *param = callee->params[index];

    return type_registry_owns(registry, param);
}

static unsigned int codegen_call_expr(CodegenState *state, ASTExpr *node) {
    size_t arg_count = node->call.args.size;

    unsigned int arg_slots = 0;
    for (size_t i = 0; i < arg_count; i++) {
        arg_slots += type_slot_count(state->registry, node->call.args.data[i]->type);
    }

    unsigned int return_slots = type_slot_count(state->registry, node->type);

    unsigned int reserved = 1 + arg_slots;
    if (return_slots > reserved) {
        reserved = return_slots;
    }

    unsigned int dest = codegen_alloc_slots(state, reserved, 1, node->span);

    unsigned int saved = dest + return_slots;

    struct {
        unsigned int slot;
        const Type *type;
    } owned_args[VM_MAX_FRAME_SLOTS];
    size_t owned_arg_count = 0;

    unsigned int offset = 1;
    for (size_t i = 0; i < arg_count; i++) {
        ASTExpr *arg = node->call.args.data[i];
        unsigned int slots = type_slot_count(state->registry, arg->type);

        unsigned int arg_reg = codegen_expr(state, arg);

        codegen_copy_slots(state, dest + offset, arg_reg, slots);

        if (expr_yields_owned(state->registry, arg) && !param_owns(state->registry, node->callee, i)) {
            assert(owned_arg_count < VM_MAX_FRAME_SLOTS && "more owned arguments than a frame has slots");

            unsigned int owner = dest + offset;

            if (offset < return_slots) {
                owner = codegen_alloc_slots(state, slots, 1, arg->span);

                codegen_copy_slots(state, owner, arg_reg, slots);
            }

            owned_args[owned_arg_count].slot = owner;
            owned_args[owned_arg_count].type = arg->type;
            owned_arg_count++;

            codegen_drop_temporary(state, arg_reg);
        }

        offset += slots;
    }

    codegen_emit_call(state, dest, node->callee, node->span);

    for (size_t i = 0; i < owned_arg_count; i++) {
        codegen_emit_release(state, owned_args[i].slot, owned_args[i].type);
    }

    codegen_release_registers(state, saved);

    return dest;
}

static void codegen_scale_by_stride(CodegenState *state, unsigned int dest, unsigned int count, size_t stride,
                                    Span span) {
    if (stride <= VM_MAX_IMMEDIATE) {
        chunk_add_instruction(state->chunk, VM_ENCODE_RK(OP_MULI, dest, count, (unsigned int)stride, 1));
        return;
    }

    unsigned int width = codegen_alloc_register(state, span);
    unsigned int const_index = constpool_add(state->chunk->const_pool, (Constant){.as_int = (int32_t)stride});

    chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_LOAD_CONST, width, const_index));
    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MULI, dest, count, width));
}

static unsigned int codegen_index_base(CodegenState *state, ASTExpr *target) {
    if (!type_is_indirect(target->type)) {
        unsigned int base = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, target->span);

        codegen_addr_of_into(state, target, base, target->span);

        return base;
    }

    unsigned int pointer = codegen_expr(state, target);

    const Type *type = target->type;

    while (type_is_indirect(type_pointee(type))) {
        unsigned int next = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, target->span);

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_LOAD_PTR_N, next, pointer, VM_INDIRECT_SLOTS));

        pointer = next;
        type = type_pointee(type);
    }

    return pointer;
}

static unsigned int codegen_index_address(CodegenState *state, ASTExpr *node) {
    const Type *array_type = node->index.array_type;
    const Type *element = type_array_element(array_type);

    unsigned int base = codegen_index_base(state, node->index.target);
    unsigned int index = codegen_expr(state, node->index.index);

    chunk_add_instruction(state->chunk, VM_ENCODE_RK(OP_BOUNDS_CHECK, base, index,
                                                     (unsigned int)type_array_length(array_type), 1));

    unsigned int offset = codegen_alloc_register(state, node->span);

    codegen_scale_by_stride(state, offset, index, type_registry_size_of(state->registry, element),
                            node->span);

    unsigned int address = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_ADD_PTR_REG, address, base, offset));

    return address;
}

static unsigned int codegen_array_lit_expr(CodegenState *state, ASTExpr *node) {
    const Type *type = node->type;
    const Type *element = type_array_element(type);

    unsigned int rd = codegen_alloc_slots(state, type_slot_count(state->registry, type),
                                          type_align_slots(state->registry, type), node->span);

    unsigned int element_slots = type_slot_count(state->registry, element);

    for (size_t i = 0; i < node->array_lit.elements.size; i++) {
        ASTExpr *value = node->array_lit.elements.data[i];

        unsigned int slot = rd + (unsigned int)i * element_slots;

        if (!codegen_expr_into(state, value, slot)) {
            codegen_copy_slots(state, slot, codegen_expr(state, value), element_slots);
        }

        codegen_own_slot(state, slot, element);
    }

    return rd;
}

static FieldTarget codegen_index_target(CodegenState *state, ASTExpr *node) {
    return (FieldTarget){.base = codegen_index_address(state, node), .offset = 0, .indirect = true};
}

static unsigned int codegen_index_expr(CodegenState *state, ASTExpr *node) {
    FieldTarget target = codegen_index_target(state, node);

    if (type_moves_as_slots(state->registry, node->type)) {
        return codegen_load_indirect_struct(state, node, node->type, target,
                                            type_slot_count(state->registry, node->type));
    }

    bool ok;
    OpCode op = field_opcode_for(type_registry_size_of(state->registry, node->type), true, true, &ok);

    if (!codegen_field_access_fits(state, node, ok, target.offset)) {
        return 0;
    }

    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, 0));

    return rd;
}

static void codegen_store_index(CodegenState *state, ASTExpr *node, unsigned int src) {
    FieldTarget target = codegen_index_target(state, node);

    if (type_moves_as_slots(state->registry, node->type)) {
        codegen_store_indirect(state, node, target, src, type_slot_count(state->registry, node->type));
        return;
    }

    bool ok;
    OpCode op = field_opcode_for(type_registry_size_of(state->registry, node->type), false, true, &ok);

    if (!codegen_field_access_fits(state, node, ok, target.offset)) {
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, target.base, src, 0));
}

static unsigned int codegen_field_expr(CodegenState *state, ASTExpr *node) {
    FieldTarget target = codegen_resolve_field_target(state, node, true);

    if (type_moves_as_slots(state->registry, node->type)) {
        if (target.indirect) {
            return codegen_load_indirect_struct(state, node, node->type, target,
                                                type_slot_count(state->registry, node->type));
        }

        return field_target_slot_count(target);
    }

    bool ok;
    OpCode op =
        field_opcode_for(type_registry_size_of(state->registry, ast_field_of(state->registry, node)->type),
                         true, target.indirect, &ok);

    if (!codegen_field_access_fits(state, node, ok, target.offset)) {
        return 0;
    }

    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, (unsigned int)target.offset));

    return rd;
}

static unsigned int codegen_addr_of_expr(CodegenState *state, ASTExpr *node) {
    ASTExpr *inner = node->unary.target;

    if (inner->kind == EXPR_DEREF) {
        return codegen_expr(state, inner->unary.target);
    }

    FieldTarget target = codegen_resolve_field_target(state, inner, false);

    unsigned int rd = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);

    if (target.offset > VM_MAX_FIELD_OFFSET) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "struct is too large for a frame");
        }

        state->failed = true;
        return rd;
    }

    OpCode op = target.indirect ? OP_ADD_PTR : OP_ADDR_OF;

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, (unsigned int)target.offset));

    return rd;
}

static unsigned int codegen_deref_expr(CodegenState *state, ASTExpr *node) {
    FieldTarget target = {
        .base = codegen_expr(state, node->unary.target),
        .offset = 0,
        .indirect = true,
    };

    unsigned int slots = type_slot_count(state->registry, node->type);

    if (type_is_struct(node->type) || slots > 1) {
        return codegen_load_indirect_struct(state, node, node->type, target, slots);
    }

    bool ok;
    OpCode op = field_opcode_for(type_registry_size_of(state->registry, node->type), true, true, &ok);

    if (!codegen_field_access_fits(state, node, ok, 0)) {
        return 0;
    }

    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, 0));

    return rd;
}

static unsigned int codegen_neg_expr(CodegenState *state, ASTExpr *node) {
    ASTExpr *inner = node->unary.target;

    if (inner->kind == EXPR_LITERAL) {
        Literal folded = inner->lit;

        if (folded.kind == TYPE_FLOAT) {
            folded.as_float = -folded.as_float;
        } else {
            folded.as_int = (int32_t)(0u - (uint32_t)folded.as_int);
        }

        unsigned int const_index = constpool_add(state->chunk->const_pool, value_from_literal(folded));
        unsigned int rd = codegen_alloc_register(state, node->span);

        chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_LOAD_CONST, rd, const_index));

        return rd;
    }

    bool is_float = type_kind(node->type) == TYPE_FLOAT;

    unsigned int operand = codegen_expr(state, inner);
    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(is_float ? OP_NEGF : OP_NEGI, rd, operand, 0));

    return rd;
}

static unsigned int codegen_not_expr(CodegenState *state, ASTExpr *node) {
    ASTExpr *inner = node->unary.target;

    if (inner->kind == EXPR_LITERAL) {
        Literal folded = inner->lit;
        folded.as_int = !folded.as_int;

        unsigned int const_index = constpool_add(state->chunk->const_pool, value_from_literal(folded));
        unsigned int rd = codegen_alloc_register(state, node->span);

        chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_LOAD_CONST, rd, const_index));

        return rd;
    }

    unsigned int false_index = constpool_add(state->chunk->const_pool, (Constant){.as_int = 0});

    unsigned int zero = codegen_alloc_register(state, node->span);
    chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_LOAD_CONST, zero, false_index));

    unsigned int operand = codegen_expr(state, inner);
    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_CMP_EQI, rd, operand, zero));

    return rd;
}

static unsigned int codegen_cast_expr(CodegenState *state, ASTExpr *node) {
    ASTExpr *operand = node->cast.operand;

    unsigned int src = codegen_expr(state, operand);

    if (type_kind(node->type) == type_kind(operand->type)) {
        return src;
    }

    unsigned int rd = codegen_alloc_register(state, node->span);
    OpCode op = type_kind(node->type) == TYPE_FLOAT ? OP_ITOF : OP_FTOI;

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, src, 0));

    return rd;
}

static unsigned int codegen_type_index(CodegenState *state, const Type *type) {
    for (size_t i = 0; i < state->unit->types.size; i++) {
        if (state->unit->types.data[i] == type) {
            return (unsigned int)i;
        }
    }

    type_list_add(&state->unit->types, type);

    heap_shape_list_add(&state->unit->type_shapes,
                        (HeapShape){
                            .size = type_registry_size_of(state->registry, type),
                            .drop = type_registry_drop_of(state->registry, type),
                            .release_width = slot_release_width(state->registry, type),
                        });

    return (unsigned int)(state->unit->types.size - 1);
}

static void codegen_emit_release(CodegenState *state, unsigned int slot, const Type *type) {
    size_t offset =
        chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_RELEASE, slot, codegen_type_index(state, type)));

    relocation_list_add(&state->unit->type_relocations,
                        (Relocation){.chunk = state->chunk, .offset = offset});
}

static unsigned int codegen_new_expr(CodegenState *state, ASTExpr *node) {
    unsigned int rd = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);

    size_t offset = chunk_add_instruction(
        state->chunk, VM_ENCODE_I(OP_NEW, rd, codegen_type_index(state, node->new_expr.type)));

    relocation_list_add(&state->unit->type_relocations,
                        (Relocation){.chunk = state->chunk, .offset = offset});

    return rd;
}

static size_t codegen_field_offset(CodegenState *state, ASTExpr *node) {
    return type_registry_layout_of(state->registry, node->field.owner)->offsets[node->field.index];
}

static FieldTarget codegen_resolve_field_target(CodegenState *state, ASTExpr *node, bool auto_deref) {
    if (node->kind == EXPR_FIELD) {
        ASTExpr *inner = node->field.target;

        if (inner->kind == EXPR_FIELD && type_is_indirect(inner->type)) {
            unsigned int base = codegen_field_expr(state, inner);

            return (FieldTarget){
                .base = base,
                .offset = codegen_field_offset(state, node),
                .indirect = true,
            };
        }

        if (inner->kind == EXPR_INDEX) {
            if (type_is_indirect(inner->type)) {
                return (FieldTarget){
                    .base = codegen_index_expr(state, inner),
                    .offset = codegen_field_offset(state, node),
                    .indirect = true,
                };
            }

            FieldTarget target = codegen_index_target(state, inner);
            target.offset += codegen_field_offset(state, node);
            return target;
        }

        FieldTarget target = codegen_resolve_field_target(state, inner, true);
        target.offset += codegen_field_offset(state, node);
        return target;
    }

    if (node->kind == EXPR_DEREF) {
        unsigned int base = codegen_expr(state, node->unary.target);

        const Type *reached = node->type;

        while (auto_deref && type_is_indirect(reached)) {
            base = codegen_load_indirect_struct(state, node, reached,
                                                (FieldTarget){
                                                    .base = base,
                                                    .offset = 0,
                                                    .indirect = true,
                                                },
                                                type_slot_count(state->registry, reached));
            reached = type_pointee(reached);
        }

        return (FieldTarget){
            .base = base,
            .offset = 0,
            .indirect = true,
        };
    }

    unsigned int base = codegen_expr(state, node);

    if (expr_yields_owned(state->registry, node)) {
        owned_list_add(&state->temporaries,
                       (OwnedSlot){.slot = base, .depth = state->depth, .type = node->type});
        codegen_record_frame_ref(state, base, node->type);
    }

    bool indirect = auto_deref && type_is_indirect(node->type);

    if (indirect) {
        const Type *reached = node->type;

        while (type_is_indirect(type_pointee(reached))) {
            base = codegen_load_indirect_struct(state, node, type_pointee(reached),
                                                (FieldTarget){
                                                    .base = base,
                                                    .offset = 0,
                                                    .indirect = true,
                                                },
                                                type_slot_count(state->registry, type_pointee(reached)));
            reached = type_pointee(reached);
        }
    }

    return (FieldTarget){
        .base = base,
        .offset = 0,
        .indirect = indirect,
    };
}

static bool codegen_field_access_fits(CodegenState *state, ASTExpr *node, bool ok, size_t offset) {
    if (ok && offset <= VM_MAX_FIELD_OFFSET) {
        return true;
    }

    if (!state->failed) {
        diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "struct is too large for a frame");
    }

    state->failed = true;
    return false;
}

static unsigned int field_target_slot_count(FieldTarget target) {
    assert(!target.indirect && "an indirect struct field has no slot of its own");
    assert(target.offset % VM_SLOT_SIZE == 0 && "a struct field is always slot-aligned");

    return target.base + (unsigned int)(target.offset / VM_SLOT_SIZE);
}

static unsigned int codegen_load_indirect_struct(CodegenState *state, ASTExpr *node, const Type *type,
                                                 FieldTarget target, unsigned int slots) {
    if (target.offset > VM_MAX_FIELD_OFFSET || slots > VM_MAX_STRUCT_SLOTS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "struct is too large for a frame");
        }

        state->failed = true;
        return 0;
    }

    unsigned int rd = codegen_alloc_slots(state, slots, type_align_slots(state->registry, type), node->span);

    unsigned int address = target.base;

    if (target.offset > 0) {
        address = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);
        chunk_add_instruction(state->chunk,
                              VM_ENCODE_R(OP_ADD_PTR, address, target.base, (unsigned int)target.offset));
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_LOAD_PTR_N, rd, address, slots));

    return rd;
}

static void codegen_store_indirect(CodegenState *state, ASTExpr *node, FieldTarget target, unsigned int src,
                                   unsigned int slots) {
    if (target.offset > VM_MAX_FIELD_OFFSET || slots > VM_MAX_STRUCT_SLOTS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "struct is too large for a frame");
        }

        state->failed = true;
        return;
    }

    unsigned int address = target.base;

    if (target.offset > 0) {
        address = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);
        chunk_add_instruction(state->chunk,
                              VM_ENCODE_R(OP_ADD_PTR, address, target.base, (unsigned int)target.offset));
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_STORE_PTR_N, address, src, slots));
}

static void codegen_store_field(CodegenState *state, ASTExpr *node, unsigned int src) {
    FieldTarget target = codegen_resolve_field_target(state, node, true);

    if (type_moves_as_slots(state->registry, node->type)) {
        if (target.indirect) {
            codegen_store_indirect(state, node, target, src, type_slot_count(state->registry, node->type));
            return;
        }

        codegen_copy_slots(state, field_target_slot_count(target), src,
                           type_slot_count(state->registry, node->type));
        return;
    }

    bool ok;
    OpCode op =
        field_opcode_for(type_registry_size_of(state->registry, ast_field_of(state->registry, node)->type),
                         false, target.indirect, &ok);

    if (!codegen_field_access_fits(state, node, ok, target.offset)) {
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, target.base, src, (unsigned int)target.offset));
}

static void codegen_store_deref(CodegenState *state, ASTExpr *node, unsigned int src) {
    FieldTarget target = {
        .base = codegen_expr(state, node->unary.target),
        .offset = 0,
        .indirect = true,
    };

    unsigned int slots = type_slot_count(state->registry, node->type);

    if (type_is_struct(node->type) || slots > 1) {
        codegen_store_indirect(state, node, target, src, slots);
        return;
    }

    bool ok;
    OpCode op = field_opcode_for(type_registry_size_of(state->registry, node->type), false, true, &ok);

    if (!codegen_field_access_fits(state, node, ok, 0)) {
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, target.base, src, 0));
}

static void codegen_addr_of_into(CodegenState *state, ASTExpr *inner, unsigned int rd, Span span) {
    FieldTarget target = codegen_resolve_field_target(state, inner, false);

    if (target.offset > VM_MAX_FIELD_OFFSET) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "struct is too large for a frame");
        }

        state->failed = true;
        return;
    }

    OpCode op = target.indirect ? OP_ADD_PTR : OP_ADDR_OF;

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, (unsigned int)target.offset));
}

static bool expr_is_immediate_operand(const ASTExpr *node, unsigned int *out) {
    if (!node || node->kind != EXPR_LITERAL || node->lit.kind != TYPE_INT) {
        return false;
    }

    if (node->lit.as_int < 0 || node->lit.as_int > VM_MAX_IMMEDIATE) {
        return false;
    }

    *out = (unsigned int)node->lit.as_int;

    return true;
}

static bool bin_op_has_constant_form(BinOp op) {
    switch (op) {
    case BIN_OP_ADD:
    case BIN_OP_SUB:
    case BIN_OP_MUL:
    case BIN_OP_DIV:
    case BIN_OP_LESS:
    case BIN_OP_GREATER:
    case BIN_OP_LEQUAL:
    case BIN_OP_GEQUAL:
    case BIN_OP_EQUAL:
    case BIN_OP_NEQUAL:
        return true;
    default:
        return false;
    }
}

static unsigned int codegen_rhs(CodegenState *state, BinOp op, ASTExpr *rhs, const Type *left_type,
                                RhsKind *kind) {
    unsigned int value = 0;

    if (type_kind(left_type) == TYPE_FLOAT) {
        if (bin_op_has_constant_form(op) && rhs->kind == EXPR_LITERAL && rhs->lit.kind == TYPE_FLOAT) {
            size_t index = constpool_add(state->chunk->const_pool, value_from_literal(rhs->lit));

            if (index <= VM_MAX_IMMEDIATE) {
                *kind = RHS_CONSTANT;
                return (unsigned int)index;
            }
        }
    } else if (expr_is_immediate_operand(rhs, &value)) {
        *kind = RHS_IMMEDIATE;
        return value;
    }

    *kind = RHS_REGISTER;

    return codegen_expr(state, rhs);
}

static OpCode bin_op_opcode_for(BinOp op, const Type *left_type, RhsKind kind) {
    if (type_is_str_ref(left_type)) {
        return op == BIN_OP_EQUAL ? OP_CMP_EQS : OP_CMP_NES;
    }

    if (kind != RHS_CONSTANT) {
        return type_kind(left_type) == TYPE_FLOAT ? bin_op_to_float_op(op) : bin_op_to_int_op(op);
    }

    switch (op) {
    case BIN_OP_ADD:
        return OP_ADDFK;
    case BIN_OP_SUB:
        return OP_SUBFK;
    case BIN_OP_MUL:
        return OP_MULFK;
    case BIN_OP_DIV:
        return OP_DIVFK;
    case BIN_OP_LESS:
        return OP_CMP_LTFK;
    case BIN_OP_GREATER:
        return OP_CMP_GTFK;
    case BIN_OP_LEQUAL:
        return OP_CMP_LEFK;
    case BIN_OP_GEQUAL:
        return OP_CMP_GEFK;
    case BIN_OP_EQUAL:
        return OP_CMP_EQFK;
    case BIN_OP_NEQUAL:
        return OP_CMP_NEFK;
    default:
        break;
    }

    assert(0 && "this operator has no constant form");
    abort();
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
    case BIN_OP_MOD:
        return OP_MODI;
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

static void codegen_emit_bin_op(CodegenState *state, ASTExpr *node, unsigned int dest, unsigned int lhs,
                                unsigned int rhs, RhsKind kind) {
    OpCode op_code = bin_op_opcode_for(node->bin_op.op, node->bin_op.left->type, kind);

    chunk_add_instruction(state->chunk, VM_ENCODE_RK(op_code, dest, lhs, rhs, kind == RHS_IMMEDIATE ? 1 : 0));
}

static unsigned int codegen_bin_op_into(CodegenState *state, ASTExpr *node, unsigned int dest) {
    unsigned int lhs = codegen_expr(state, node->bin_op.left);

    RhsKind rhs_kind = RHS_REGISTER;
    unsigned int rhs =
        codegen_rhs(state, node->bin_op.op, node->bin_op.right, node->bin_op.left->type, &rhs_kind);

    codegen_emit_bin_op(state, node, dest, lhs, rhs, rhs_kind);

    return dest;
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

    RhsKind rhs_kind = RHS_REGISTER;
    unsigned int rhs =
        codegen_rhs(state, node->bin_op.op, node->bin_op.right, node->bin_op.left->type, &rhs_kind);

    unsigned int result = codegen_alloc_register(state, node->span);

    codegen_emit_bin_op(state, node, result, lhs, rhs, rhs_kind);

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

static CodegenLabel codegen_create_label(CodegenState *state) {
    return (CodegenLabel){.position = chunk_add_instruction(state->chunk, 0)};
}

static void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg) {
    Instruction patch = VM_ENCODE_I(op, reg, state->chunk->instructions.size - label.position - 1);
    chunk_patch_instruction(state->chunk, label.position, patch);
}

static void codegen_emit_loop(CodegenState *state, size_t target) {
    size_t position = chunk_add_instruction(state->chunk, 0);
    ptrdiff_t offset = (ptrdiff_t)target - (ptrdiff_t)(position + 1);

    chunk_patch_instruction(state->chunk, position, VM_ENCODE_I(OP_JMP, 0, offset));
}

static bool expr_yields_owned(TypeRegistry *registry, const ASTExpr *expr) {
    if (!expr || type_registry_copies(registry, expr->type)) {
        return false;
    }

    switch (expr->kind) {
    case EXPR_BIN_OP:
        return false;

    case EXPR_NEW:
    case EXPR_CALL:
        return true;

    case EXPR_VARIABLE:
        return expr->moves;

    default:
        return false;
    }
}

static void codegen_record_frame_ref(CodegenState *state, unsigned int slot, const Type *type) {
    for (size_t i = 0; i < state->frame_refs.size; i++) {
        if (state->frame_refs.data[i].slot == slot) {
            return;
        }
    }

    frame_ref_list_add(&state->frame_refs, (FrameRef){
                                               .slot = slot,
                                               .drop = type_registry_drop_of(state->registry, type),
                                               .release_width = slot_release_width(state->registry, type),
                                           });
}

static void codegen_own_slot_at(CodegenState *state, unsigned int slot, const Type *type,
                                unsigned int depth) {
    assert(!(type && type_kind(type) == TYPE_REF) && "a 'ref T' slot never owns what it names");

    owned_list_add(&state->owned, (OwnedSlot){.slot = slot, .depth = depth, .type = type});

    codegen_record_frame_ref(state, slot, type);
}

static void codegen_own_slot(CodegenState *state, unsigned int slot, const Type *type) {
    codegen_own_slot_at(state, slot, type, state->depth);
}

static void codegen_disown_slot(CodegenState *state, unsigned int slot) {
    for (size_t i = 0; i < state->owned.size; i++) {
        if (state->owned.data[i].slot == slot) {
            for (size_t j = i + 1; j < state->owned.size; j++) {
                state->owned.data[j - 1] = state->owned.data[j];
            }

            state->owned.size--;
            return;
        }
    }
}

static bool codegen_slot_is_owned(const CodegenState *state, unsigned int slot) {
    for (size_t i = 0; i < state->owned.size; i++) {
        if (state->owned.data[i].slot == slot) {
            return true;
        }
    }

    return false;
}

static void codegen_release_owned(CodegenState *state, unsigned int keep_depth, unsigned int moved) {
    while (state->owned.size > 0 && state->owned.data[state->owned.size - 1].depth > keep_depth) {
        OwnedSlot owned = state->owned.data[--state->owned.size];

        if (owned.slot == moved) {
            continue;
        }

        codegen_emit_release(state, owned.slot, owned.type);
    }
}

static void codegen_emit_releases_below(CodegenState *state, unsigned int keep_depth) {
    for (size_t i = state->owned.size; i > 0; i--) {
        OwnedSlot owned = state->owned.data[i - 1];

        if (owned.depth <= keep_depth) {
            break;
        }

        codegen_emit_release(state, owned.slot, owned.type);
    }
}

static unsigned int codegen_slot_of(CodegenState *state, Binding *binding) {
    SlotBinding *slot = slot_map_lookup(state->slots, binding);

    assert(slot && "binding was never assigned a frame slot");

    return slot->slot;
}

static unsigned int codegen_decl_depth_of(CodegenState *state, Binding *binding) {
    SlotBinding *slot = slot_map_lookup(state->slots, binding);

    assert(slot && "binding was never assigned a frame slot");

    return slot->depth;
}

static void codegen_set_slot(CodegenState *state, Binding *binding, unsigned int slot) {
    slot_map_insert(state->slots, binding, (SlotBinding){.slot = slot, .depth = state->depth});
}

static unsigned int codegen_alloc_register(CodegenState *state, Span span) {
    return codegen_alloc_slots(state, 1, 1, span);
}

static unsigned int codegen_alloc_slots(CodegenState *state, unsigned int count, unsigned int align_slots,
                                        Span span) {
    if (align_slots > 1 && state->next_reg % align_slots != 0) {
        state->next_reg += align_slots - state->next_reg % align_slots;
    }

    if (count > VM_MAX_FRAME_SLOTS || state->next_reg > VM_MAX_FRAME_SLOTS - count) {
        if (!state->failed) {
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

static void codegen_release_registers(CodegenState *state, unsigned int saved) { state->next_reg = saved; }

static void codegen_copy_slots(CodegenState *state, unsigned int dest, unsigned int src, unsigned int count) {
    if (dest == src) {
        return;
    }

    if (count > 1 && count <= VM_MAX_MOVE_SLOTS) {
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE_N, dest, src, count));
        return;
    }

    for (unsigned int i = 0; i < count; i++) {
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, dest + i, src + i, 0));
    }
}

static size_t slot_release_width(TypeRegistry *registry, const Type *type) {
    return type_registry_size_of(registry, type);
}

static unsigned int type_slot_count(TypeRegistry *registry, const Type *type) {
    if (!type) {
        return 1;
    }

    return (unsigned int)((type_registry_size_of(registry, type) + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);
}

static unsigned int type_align_slots(TypeRegistry *registry, const Type *type) {
    if (!type || type_registry_align_of(registry, type) <= VM_SLOT_SIZE) {
        return 1;
    }

    return (unsigned int)(type_registry_align_of(registry, type) / VM_SLOT_SIZE);
}

static bool type_is_struct(const Type *type) { return type && type_kind(type) == TYPE_STRUCT; }

static bool type_owns_through_members(TypeRegistry *registry, const Type *type) {
    return type_registry_owns(registry, type) && !type_is_indirect(type);
}

static bool type_moves_as_slots(TypeRegistry *registry, const Type *type) {
    const TypeFields *fields = type_registry_fields_of(registry, type);

    return type_is_indirect(type) || type_owns_through_an_address(type) || fields->count > 0;
}

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
