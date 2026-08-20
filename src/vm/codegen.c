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

// Where a variable or parameter lives in the frame being generated. Keyed by
// Symbol * because a Symbol is what a name resolved to, but held here rather
// than on the Symbol itself: a slot is true only of one compile, while a
// top-level Symbol outlives every compile and is what a GabFunc handle points
// at. Writing the slot onto the Symbol would let a recompile silently change
// the frame layout under a handle a host is still holding.
#define slot_map_hash(key) (size_t)key
#define slot_map_key_equals(key, other) key == other
#define slot_map_key_dup(key) key
#define slot_map_entry_free(key, value)

GAB_HASH_MAP(SlotMap, slot_map, Symbol *, unsigned int)

#define SLOT_MAP_INITIAL_CAPACITY 16

// A slot holding an owned reference, and the block depth that declared it.
// Kept as a stack because blocks nest and close in order.
typedef struct {
    unsigned int slot;
    unsigned int depth;

    // What the slot holds. Kept for the assert in codegen_own_slot and for
    // reading a chunk back; freeing is one opcode whatever the type.
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

// The loop a 'break' or a 'continue' belongs to. Both jump forward to a place
// not yet emitted, so each records a label the loop patches once it knows where
// its two ends landed.
typedef struct LoopContext {
    CodegenLabelList breaks;
    CodegenLabelList continues;

    // Block depth just inside the loop, so a jump knows which owned slots it is
    // leaving behind and has to release.
    unsigned int depth;
} LoopContext;

typedef struct {
    Chunk *chunk;
    unsigned int next_reg;

    // next_reg falls back at scope exits, so the frame is sized from the
    // highest slot ever reached rather than the final value.
    unsigned int max_reg;

    CodegenOutput output;

    // Frame-local, so it is per function body: a nested function generates
    // against its own, and the outer one's slots are not visible in it.
    SlotMap *slots;

    // Slots holding an owned '*T', innermost block last. Released where the
    // block that declared them closes, rather than where the frame pops:
    // sibling blocks reuse slots, so a per-frame table could say a slot is
    // sometimes a pointer but never when, and a pop after the reuse would
    // release whatever replaced it. Releasing at the close happens while the
    // slot still holds what it was declared as.
    OwnedList owned;

    // Every slot this function ever owns a reference in, for the unwinder. See
    // FuncPrototype::refs.
    FrameRefList frame_refs;

    // How deep the block nesting is, so a close knows which owned slots were
    // declared by the block it is closing.
    unsigned int depth;

    // The innermost enclosing loop, or NULL outside one. A function body starts
    // from NULL: the resolver has already refused a jump that would leave one.
    LoopContext *loop;

    Diagnostics *diagnostics;
    bool failed;
} CodegenState;

// Whether a value of this type carries a reference that has to be released. A
// scalar never does, which is why an int costs nothing at runtime: the question
// is settled at compile time from the static type.
static bool type_is_owned(const Type *type);

// Records that 'slot' owns an object for as long as the current block runs.
static void codegen_own_slot(CodegenState *state, unsigned int slot, const Type *type);

// Whether this slot already owns a reference, and so has one to drop when it is
// overwritten.
static bool codegen_slot_is_owned(const CodegenState *state, unsigned int slot);

// Emits a release for every owned slot the current block declared, and drops
// them. 'moved' is a slot whose ownership is leaving the frame — a returned
// pointer — and is skipped; pass VM_INVALID_REGISTER when nothing is moving.
static void codegen_release_owned(CodegenState *state, unsigned int keep_depth, unsigned int moved);

// Whether an expression hands its caller a reference to own, or merely lends
// one it keeps.
static bool expr_yields_owned(const ASTExpr *expr);

// The frame slot a symbol was given. Every read is of a slot this same
// function body assigned, so a miss is a codegen bug rather than a user error.
static unsigned int codegen_slot_of(CodegenState *state, Symbol *symbol) {
    unsigned int *slot = slot_map_lookup(state->slots, symbol);

    assert(slot && "symbol was never assigned a frame slot");

    return *slot;
}

static void codegen_set_slot(CodegenState *state, Symbol *symbol, unsigned int slot) {
    slot_map_insert(state->slots, symbol, slot);
}

static void codegen_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_reserve_proto(CodegenState *state, ASTFuncDecl *ast);
static void codegen_return_stmt(CodegenState *state, ASTReturnStmt *ast);
static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast);
static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast);
static void codegen_compound_assign_stmt(CodegenState *state, ASTCompoundAssignStmt *ast);
static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast);
static void codegen_if_stmt(CodegenState *state, ASTIfStmt *ast);
static void codegen_for_stmt(CodegenState *state, ASTForStmt *ast);
static void codegen_jump_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_func_decl_stmt(CodegenState *state, ASTFuncDecl *ast);

static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast);
static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_logical_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_into(CodegenState *state, ASTExpr *node, unsigned int dest);
static unsigned int codegen_variable_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_call_expr(CodegenState *state, ASTExpr *node);
static void codegen_emit_call(CodegenState *state, unsigned int dest, const Symbol *callee, Span span);
static unsigned int codegen_new_expr(CodegenState *state, ASTExpr *node);
static void codegen_addr_of_into(CodegenState *state, ASTExpr *inner, unsigned int rd, Span span);

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
static unsigned int codegen_neg_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_not_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_cast_expr(CodegenState *state, ASTExpr *node);
static void codegen_store_deref(CodegenState *state, ASTExpr *node, unsigned int src);
static void codegen_copy_slots(CodegenState *state, unsigned int dest, unsigned int src, unsigned int count);

// A value occupies ceil(size / 4) consecutive slots, which is 1 for every
// scalar.
static unsigned int type_slot_count(const Type *type) {
    if (!type) {
        return 1;
    }

    return (unsigned int)((type->size + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);
}

// Slot alignment a value of this type needs. A scalar wants one; an 8-byte
// pointer wants two, so that it lands on its natural alignment.
static unsigned int type_align_slots(const Type *type) {
    if (!type || type->alignment <= VM_SLOT_SIZE) {
        return 1;
    }

    return (unsigned int)(type->alignment / VM_SLOT_SIZE);
}

static bool type_is_struct(const Type *type) { return type && type->kind == TYPE_STRUCT; }

// Whether a field is moved as a run of slots rather than through a width-tagged
// field opcode. A struct is, because its slots are laid out inline — and so is
// a pointer, which is 8 bytes and has no 8-wide opcode. Before heap objects
// existed no struct could hold a pointer, so the two cases only meet now.
static bool type_moves_as_slots(const Type *type) { return type_is_struct(type) || type_is_pointer(type); }

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

CodegenLabel codegen_create_label(CodegenState *state);
void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg);

Chunk *codegen_generate(ASTScript *script, CodegenOutput output, Diagnostics *diagnostics,
                        CodegenFrameInfo *frame_info) {
    CodegenState state = {
        .chunk = chunk_create(),
        .next_reg = 0,
        .max_reg = 0,
        .output = output,
        .slots = slot_map_create(SLOT_MAP_INITIAL_CAPACITY),
        .owned = owned_list_create(),
        .depth = 0,
        .frame_refs = frame_ref_list_create(),
        .diagnostics = diagnostics,
        .failed = false,
    };

    // Prototype indices first, bodies second — mirroring the resolver, which
    // hoists declarations for the same reason. A body may call a function
    // declared below it, and the OP_CALL it emits needs that function's index
    // before its body has been reached.
    for (size_t i = 0; i < script->statements.size; i++) {
        ASTStmt *stmt = script->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL) {
            codegen_reserve_proto(&state, &stmt->func_decl);
        }
    }

    for (size_t i = 0; i < script->statements.size; i++) {
        codegen_stmt(&state, script->statements.data[i]);
    }

    // The slots were only ever true of this compile, so they go with it.
    slot_map_destroy(state.slots);
    owned_list_free(&state.owned);

    if (state.failed) {
        chunk_free(state.chunk);
        frame_ref_list_free(&state.frame_refs);
        return NULL;
    }

    // The refs outlive the compile, unlike everything above: the top-level
    // frame is unwound through them, so ownership passes to the caller with the
    // chunk.
    if (frame_info) {
        frame_info->max_registers = state.max_reg;
        frame_info->refs = state.frame_refs;
    } else {
        frame_ref_list_free(&state.frame_refs);
    }

    return state.chunk;
}

static void codegen_stmt(CodegenState *state, ASTStmt *ast) {
    unsigned int saved = state->next_reg;

    switch (ast->kind) {
    case STMT_EXPR: {
        unsigned int reg = codegen_expr(state, ast->expr.value);

        // 'new Player;' on its own line owns a reference nothing will ever
        // store, so the statement is where it dies. Without this the object
        // leaks with no name left to release it by.
        if (expr_yields_owned(ast->expr.value)) {
            chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RELEASE, reg, 0, 0));
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

    // Everything the function owns dies here, so it is released before the
    // return rather than at each block's close, which this jumps past. The
    // returned reference is *moved*: ownership passes to the caller, so
    // releasing its slot would free the object before the caller sees it.
    //
    // Depth 0 is the function body, so releasing to depth 0 covers every block
    // this return escapes, including the body itself.
    codegen_release_owned(state, 0, ast->result ? reg : VM_INVALID_REGISTER);

    if (slots == 1) {
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN, 0, reg, 0));
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RETURN_N, 0, reg, slots));
}

static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast) {
    Span span = ast->initializer ? ast->initializer->span : (Span){0};

    codegen_set_slot(state, ast->symbol,
                     codegen_alloc_slots(state, type_slot_count(ast->symbol->var.type),
                                         type_align_slots(ast->symbol->var.type), span));

    // A 'ref T' local borrows: nothing frees it, so its slot is never owned and
    // never listed on the frame. It needs no null-init either — nothing will
    // read it as an owner.
    bool is_ref = ast->symbol->var.type && ast->symbol->var.type->is_ref;

    if (!ast->initializer) {
        return;
    }

    // The variable's own slot is already reserved; everything the initializer
    // allocates above it is a temporary and is reclaimed here.
    unsigned int saved = state->next_reg;

    unsigned int r1 = codegen_expr(state, ast->initializer);

    unsigned int slot = codegen_slot_of(state, ast->symbol);

    codegen_copy_slots(state, slot, r1, type_slot_count(ast->symbol->var.type));

    // The variable takes over the initializer's object, so an owned result is
    // now owned by this slot. A borrowed one — reading another variable — is
    // left alone: the slot it came from still owns it, and freeing here would
    // free it twice. A 'ref T' slot never owns, whatever it was given.
    if (!is_ref && expr_yields_owned(ast->initializer)) {
        codegen_own_slot(state, slot, ast->symbol->var.type);
    }

    codegen_release_registers(state, saved);
}

static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast) {
    // Storing into a field, or through a pointer, puts a value somewhere that
    // outlives the statement. An owning target takes over what it is given and
    // frees whatever it held before; a 'ref T' target owns nothing either way,
    // so it falls through to the ordinary store below.
    bool target_owns = (ast->target->kind == EXPR_FIELD || ast->target->kind == EXPR_DEREF) &&
                       type_is_owned(ast->target->type) && !ast->target->type->is_ref;

    if (target_owns) {
        // Under unique ownership an owning field holds the only reference to
        // what it names, so the value stored has to be one nothing else owns:
        // 'new', or a call handing its result over. A borrow — a variable, a
        // parameter, another field — is refused, because storing it would make
        // two owners of one object and neither would know about the other.
        //
        // 'ref T' is how a field names something it does not own, and that is
        // the target kind this branch has already excluded.
        if (!expr_yields_owned(ast->value)) {
            if (!state->failed) {
                diag_error(state->diagnostics, GAB_ERR_CODEGEN, ast->value->span,
                           "cannot store a borrowed value in an owning field; declare the field 'ref' to "
                           "name something it does not own");
            }

            state->failed = true;
            return;
        }

        // Read what the destination holds before overwriting it, and free it
        // after the store rather than before: the value being stored is fresh,
        // but freeing first would still leave a window where the field points
        // at freed memory if anything observed it.
        unsigned int old = ast->target->kind == EXPR_FIELD ? codegen_field_expr(state, ast->target)
                                                           : codegen_deref_expr(state, ast->target);

        unsigned int src = codegen_expr(state, ast->value);

        if (ast->target->kind == EXPR_FIELD) {
            codegen_store_field(state, ast->target, src);
        } else {
            codegen_store_deref(state, ast->target, src);
        }

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RELEASE, old, 0, 0));
        return;
    }

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

    // A binary op computes straight into the target instead of into a temporary
    // this would then have to copy down. The logical ops are excluded because
    // they short-circuit through a jump and own their result register.
    if (ast->value->kind == EXPR_BIN_OP && ast->value->bin_op.op != BIN_OP_AND &&
        ast->value->bin_op.op != BIN_OP_OR) {
        codegen_bin_op_into(state, ast->value, rd);
        return;
    }

    unsigned int r1 = codegen_expr(state, ast->value);

    bool target_is_ref = ast->target->type && ast->target->type->is_ref;

    // Reassigning a variable that owns an object: it frees the old one and takes
    // over the new. Freeing after the store, for the same reason a field store
    // does — 'p = p' must not free what it is about to keep.
    //
    // A 'ref T' slot owns nothing, so it is a plain store however it is written.
    if (!target_is_ref && type_is_owned(ast->target->type) && codegen_slot_is_owned(state, rd)) {
        // A borrow cannot take over a slot that owns: the old object would be
        // freed and the slot left naming something a different slot still owns.
        if (!expr_yields_owned(ast->value)) {
            if (!state->failed) {
                diag_error(state->diagnostics, GAB_ERR_CODEGEN, ast->value->span,
                           "cannot assign a borrowed value to an owning variable; declare it 'ref' to name "
                           "something it does not own");
            }

            state->failed = true;
            return;
        }

        unsigned int old = codegen_alloc_slots(state, VM_POINTER_SLOTS, VM_POINTER_SLOTS, ast->target->span);

        codegen_copy_slots(state, old, rd, VM_POINTER_SLOTS);
        codegen_copy_slots(state, rd, r1, VM_POINTER_SLOTS);
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RELEASE, old, 0, 0));
        return;
    }

    codegen_copy_slots(state, rd, r1, type_slot_count(ast->target->type));

    // A borrowed value assigned into a slot that owns nothing yet leaves the
    // slot borrowing too, so there is nothing to record. An owned one makes the
    // slot an owner from here on.
    if (!target_is_ref && type_is_owned(ast->target->type) && expr_yields_owned(ast->value)) {
        codegen_own_slot(state, rd, ast->target->type);
    }
}

static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast) {
    unsigned int saved = state->next_reg;
    unsigned int enclosing_depth = state->depth++;

    for (size_t i = 0; i < ast->list.size; i++) {
        codegen_stmt(state, ast->list.data[i]);
    }

    // Released before the slots are reclaimed, so each still holds what it was
    // declared as. A block ending in 'return' has already released these on the
    // way out, and the list is empty by now.
    codegen_release_owned(state, enclosing_depth, VM_INVALID_REGISTER);

    state->depth = enclosing_depth;
    codegen_release_registers(state, saved);
}

// Emits a release for every owned slot deeper than keep_depth without dropping
// the entries, for a jump that leaves those blocks early. The blocks it jumped
// out of still close normally on the path that falls through, and that close is
// what pops them.
static void codegen_emit_releases_below(CodegenState *state, unsigned int keep_depth) {
    for (size_t i = state->owned.size; i > 0; i--) {
        OwnedSlot owned = state->owned.data[i - 1];

        if (owned.depth <= keep_depth) {
            break;
        }

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RELEASE, owned.slot, 0, 0));
    }
}

// Jumps back to an instruction index already emitted. The offset is negative,
// which an ordinary OP_JMP carries: it is measured from the instruction after
// this one, the point the interpreter has reached by the time it jumps.
static void codegen_emit_loop(CodegenState *state, size_t target) {
    size_t position = chunk_add_instruction(state->chunk, 0);
    ptrdiff_t offset = (ptrdiff_t)target - (ptrdiff_t)(position + 1);

    chunk_patch_instruction(state->chunk, position, VM_ENCODE_I(OP_JMP, 0, offset));
}

static void codegen_for_stmt(CodegenState *state, ASTForStmt *ast) {
    // The initializer's own scope, holding it for the whole loop: it is
    // declared once, outlives every iteration, and dies when the loop does.
    unsigned int saved = state->next_reg;
    unsigned int enclosing_depth = state->depth++;

    if (ast->init) {
        codegen_stmt(state, ast->init);
    }

    LoopContext *enclosing_loop = state->loop;
    LoopContext loop = {
        .breaks = codegen_label_list_create(),
        .continues = codegen_label_list_create(),

        // Slots declared inside the loop are released by a jump that leaves
        // them; the initializer's are not, since it outlives the body.
        .depth = state->depth,
    };
    state->loop = &loop;

    size_t condition_target = state->chunk->instructions.size;

    CodegenLabel exit_label = {0};
    unsigned int exit_reg = 0;
    unsigned int condition_saved = state->next_reg;

    if (ast->condition) {
        exit_reg = codegen_expr(state, ast->condition);
        exit_label = codegen_create_label(state);

        // Reclaimed before the body so each iteration reuses the slot rather
        // than the frame growing per loop.
        codegen_release_registers(state, condition_saved);
    }

    codegen_stmt(state, ast->body);

    // 'continue' lands on the post clause, so the three-clause form advances
    // before it tests again.
    for (size_t i = 0; i < loop.continues.size; i++) {
        codegen_patch_jump(state, loop.continues.data[i], OP_JMP, 0);
    }

    if (ast->post) {
        codegen_stmt(state, ast->post);
    }

    codegen_emit_loop(state, condition_target);

    // Patched only now: a forward jump's offset is measured to the end of the
    // chunk as it stands, which is this point for both the failed condition and
    // every 'break'.
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

    // What the jump skips past still has to be freed, and the ordinary close of
    // those blocks is on the path this jump avoids.
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

// Claims the prototype index a function's OP_CALL will encode, without
// generating anything. Reserving it before any body is generated is what lets a
// body call a function whose own body has not been reached yet — the recursion
// case, and now the forward-call case the resolver's hoisting admits.
//
// Durable, unlike a frame slot: this is what OP_CALL encodes and what a host's
// handle resolves through after the compile is long gone.
static void codegen_reserve_proto(CodegenState *state, ASTFuncDecl *ast) {
    if (!ast->symbol || ast->symbol->func.proto_index != SYMBOL_FUNC_NO_PROTO) {
        return;
    }

    func_proto_list_add(state->output.funcs, (FuncPrototype){0});
    ast->symbol->func.proto_index = state->output.funcs->size - 1;
}

static void codegen_func_decl_stmt(CodegenState *state, ASTFuncDecl *ast) {
    Chunk *func_chunk = chunk_create();

    // Reserved by the pre-pass for a top-level function; a nested one, which no
    // pre-pass saw, reserves its own here.
    codegen_reserve_proto(state, ast);

    size_t proto_index = ast->symbol->func.proto_index;

    unsigned int func_next_reg = 1;

    // The body generates against its own frame, so its slots are its own.
    CodegenState func_state = (CodegenState){
        .chunk = func_chunk,
        .output = state->output,
        .slots = slot_map_create(SLOT_MAP_INITIAL_CAPACITY),
        .owned = owned_list_create(),
        .depth = 0,
        .frame_refs = frame_ref_list_create(),
        .diagnostics = state->diagnostics,
        .failed = false,
    };

    // The receiver is parameter zero, so it takes the first slot above the
    // return slot and every declared parameter shifts up past it.
    if (ast->receiver && ast->receiver->symbol) {
        Symbol *receiver = ast->receiver->symbol;

        codegen_set_slot(&func_state, receiver, func_next_reg);
        func_next_reg += type_slot_count(receiver->var.type);
    }

    // A multi-slot parameter owns consecutive slots starting at its base, so
    // the callee addresses it by that slot exactly like a local.
    for (size_t i = 0; i < ast->params.size; i++) {
        Symbol *param = ast->params.data[i]->symbol;

        codegen_set_slot(&func_state, param, func_next_reg);
        func_next_reg += type_slot_count(param->var.type);
    }

    func_state.next_reg = func_next_reg;
    func_state.max_reg = func_next_reg;

    codegen_stmt(&func_state, ast->body);

    state->failed = state->failed || func_state.failed;

    OpCode last = func_chunk->instructions.size > 0
                      ? VM_DECODE_OPCODE(instruction_list_back(&func_chunk->instructions))
                      : OP_LOAD_CONST;

    if (func_chunk->instructions.size == 0 || (last != OP_RETURN && last != OP_RETURN_N)) {
        chunk_add_instruction(func_chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));
    }

    func_proto_list_emplace(state->output.funcs, proto_index,
                            (FuncPrototype){
                                .chunk = func_chunk,
                                // The receiver is parameter zero, so it counts.
                                .arity = ast->params.size + (ast->receiver ? 1 : 0),
                                .max_registers = func_state.max_reg,
                                .refs = func_state.frame_refs,
                            });

    owned_list_free(&func_state.owned);
    slot_map_destroy(func_state.slots);
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
    case EXPR_FIELD:
        return codegen_field_expr(state, ast);
    case EXPR_ADDR_OF:
        return codegen_addr_of_expr(state, ast);
    case EXPR_DEREF:
        return codegen_deref_expr(state, ast);
    case EXPR_NEG:
        return codegen_neg_expr(state, ast);
    case EXPR_NOT:
        return codegen_not_expr(state, ast);
    case EXPR_CAST:
        return codegen_cast_expr(state, ast);
    case EXPR_NEW:
        return codegen_new_expr(state, ast);
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

static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node) {
    unsigned int const_index = constpool_add(state->chunk->const_pool, value_from_literal(node->lit));
    unsigned int r1 = codegen_alloc_register(state, node->span);
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, r1, const_index);

    chunk_add_instruction(state->chunk, load_const);

    return r1;
}

// 'new T' allocates and leaves an owned '*T' in a pointer-sized destination.
// The type travels by index because a Type * is 8 bytes and cannot ride in an
// instruction; the list is interned by pointer identity, which the type system
// already guarantees.
static unsigned int codegen_new_expr(CodegenState *state, ASTExpr *node) {
    unsigned int rd = codegen_alloc_slots(state, VM_POINTER_SLOTS, VM_POINTER_SLOTS, node->span);

    size_t type_index = state->output.heap_types->size;

    for (size_t i = 0; i < state->output.heap_types->size; i++) {
        if (state->output.heap_types->data[i] == node->new_expr.type) {
            type_index = i;
            break;
        }
    }

    if (type_index == state->output.heap_types->size) {
        type_list_add(state->output.heap_types, node->new_expr.type);
    }

    // Masking alone would allocate the wrong type, so an index too wide for
    // the field is rejected rather than truncated.
    if (type_index > VM_MAX_HEAP_TYPES) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span,
                       "too many allocated types in one program");
        }

        state->failed = true;
        return rd;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_NEW, rd, (unsigned int)type_index));

    return rd;
}

// The OP_CALL itself, shared by a plain call and a method call: by this point
// the two have laid out an identical block and differ in nothing the
// instruction records.
//
// I-type, so the prototype index gets the 17-bit field. It is not a register,
// and while it rode in an 8-bit one a single VM could hold only 255 functions
// across every module it ever loaded. No argument count is encoded: the
// callee's frame is based at dest, so the arguments written above dest already
// are its parameters, and its size comes from the prototype.
static void codegen_emit_call(CodegenState *state, unsigned int dest, const Symbol *callee, Span span) {
    // Masking alone would encode a call to the wrong function, so an index too
    // wide for the field is rejected rather than truncated.
    if (callee->func.proto_index > VM_MAX_PROTOTYPES) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "too many functions in one program");
        }

        state->failed = true;
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_CALL, dest, (unsigned int)callee->func.proto_index));
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

    // Argument slots holding an object nothing else owns, so they can be freed
    // once the call returns. A parameter borrows, so the callee frees nothing;
    // an owned temporary would otherwise belong to nobody the moment the
    // argument block is reclaimed.
    //
    // Bounded by the frame rather than by the argument count: the block was
    // reserved above, and codegen_alloc_slots refuses one wider than a frame,
    // so there can never be more owned arguments than slots to hold them.
    unsigned int owned_args[VM_MAX_FRAME_SLOTS];
    size_t owned_arg_count = 0;

    unsigned int offset = 1;
    for (size_t i = 0; i < arg_count; i++) {
        ASTExpr *arg = node->call.args.data[i];
        unsigned int slots = type_slot_count(arg->type);

        unsigned int arg_reg = codegen_expr(state, arg);

        codegen_copy_slots(state, dest + offset, arg_reg, slots);

        // Recorded against the argument block rather than the expression's own
        // register: the copy above is what the callee reads, and the source
        // register may be reclaimed before the free is emitted.
        if (expr_yields_owned(arg)) {
            assert(owned_arg_count < VM_MAX_FRAME_SLOTS && "more owned arguments than a frame has slots");

            owned_args[owned_arg_count++] = dest + offset;
        }

        offset += slots;
    }

    codegen_emit_call(state, dest, node->symbol, node->span);

    // After the call, so the callee still has its arguments, and before the
    // registers are reclaimed, so the slots still hold what was put in them.
    for (size_t i = 0; i < owned_arg_count; i++) {
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RELEASE, owned_args[i], 0, 0));
    }

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
        ASTExpr *inner = node->field.target;

        // A pointer-typed field ends the chain rather than extending it: from
        // there the struct lives wherever that pointer says, so the pointer has
        // to be loaded and followed. Summing the offset instead would address
        // the pointer's own slot as if its pointee were inline in it.
        //
        // Reachable only since a struct could hold a pointer, which is why
        // 'o.child.n' is the first expression to need this: it loads o.child,
        // then addresses n from there.
        if (inner->kind == EXPR_FIELD && type_is_pointer(inner->type)) {
            unsigned int base = codegen_field_expr(state, inner);

            return (FieldTarget){
                .base = base,
                .offset = node->field.field->offset,
                .indirect = true,
            };
        }

        // Everything else below a field access is a struct being reached into,
        // so a pointer there is always reached through.
        FieldTarget target = codegen_field_base(state, inner, true);
        target.offset += node->field.field->offset;
        return target;
    }

    if (node->kind == EXPR_DEREF) {
        unsigned int base = codegen_expr(state, node->unary.target);

        return (FieldTarget){
            .base = base,
            .offset = 0,
            .indirect = true,
        };
    }

    unsigned int base = codegen_expr(state, node);
    bool indirect = auto_deref && type_is_pointer(node->type);

    // Only a pointer actually reached through needs checking. '&w' names the
    // slot rather than following it, and is fine whether or not the object is
    // still there.
    if (indirect) {
    }

    return (FieldTarget){
        .base = base,
        .offset = 0,
        .indirect = indirect,
    };
}

// An offset rides in an 8-bit operand, and only 1, 2 and 4 byte fields have an
// opcode. Both are compile-time facts, so a violation is reported once here.
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

// The slots a struct occupies, addressed directly. Only valid for a direct
// target: through a pointer there is no slot to name.
static unsigned int codegen_field_slots(FieldTarget target) {
    assert(!target.indirect && "an indirect struct field has no slot of its own");
    assert(target.offset % VM_SLOT_SIZE == 0 && "a struct field is always slot-aligned");

    return target.base + (unsigned int)(target.offset / VM_SLOT_SIZE);
}

// Copies a struct out of the address a pointer holds into fresh slots, so the
// result reads like any other struct-valued expression.
// 'type' is what lands in the destination, which is not always node->type: a
// method call derefs its receiver, and the node's own type is the return type.
static unsigned int codegen_load_indirect_struct(CodegenState *state, ASTExpr *node, const Type *type,
                                                 FieldTarget target, unsigned int slots) {
    if (target.offset > VM_MAX_FIELD_OFFSET || slots > VM_MAX_STRUCT_SLOTS) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, node->span, "struct is too large for a frame");
        }

        state->failed = true;
        return 0;
    }

    unsigned int rd = codegen_alloc_slots(state, slots, type_align_slots(type), node->span);

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

    // A multi-slot field is addressed, not loaded: its slots are already laid
    // out inline, so the caller reads them where they sit. Through a pointer
    // there are no such slots, so it is copied out instead.
    if (type_moves_as_slots(node->type)) {
        if (target.indirect) {
            return codegen_load_indirect_struct(state, node, node->type, target, type_slot_count(node->type));
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

    if (type_moves_as_slots(node->type)) {
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
    if (target.offset > VM_MAX_FIELD_OFFSET || slots > VM_MAX_STRUCT_SLOTS) {
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
// The address of 'inner' written into 'rd'. Split from codegen_addr_of_expr so
// a method call can put a receiver's address straight into the argument slot it
// reserved, rather than into a slot of this function's choosing.
static void codegen_addr_of_into(CodegenState *state, ASTExpr *inner, unsigned int rd, Span span) {
    // '&p' where p is a pointer names p itself, so the chain must stop at it
    // rather than reach through.
    FieldTarget target = codegen_field_base(state, inner, false);

    if (target.offset > VM_MAX_FIELD_OFFSET) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "struct is too large for a frame");
        }

        state->failed = true;
        return;
    }

    // Through a pointer the base is already an address, so the field offset is
    // added to it rather than to a slot index.
    OpCode op = target.indirect ? OP_ADD_PTR : OP_ADDR_OF;

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, (unsigned int)target.offset));
}

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

    if (target.offset > VM_MAX_FIELD_OFFSET) {
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

// Negation lowers to a subtraction from zero rather than earning an opcode:
// that is exactly what it is on the machine, and a dedicated OP_NEG would add
// a dispatch case per numeric type without saving an instruction.
//
// The zero has to live in a register. Only the second operand of an arithmetic
// instruction may be immediate, and the zero here is the first.
static unsigned int codegen_neg_expr(CodegenState *state, ASTExpr *node) {
    ASTExpr *inner = node->unary.target;

    // '-5' is a constant, so it is folded into one rather than emitted as a
    // load of 5 and a subtraction. This is also the only way a negative
    // literal reaches the constant pool, since the lexer only ever scans
    // digits and leaves the sign to this node.
    if (inner->kind == EXPR_LITERAL) {
        Literal folded = inner->lit;

        if (folded.kind == TYPE_FLOAT) {
            folded.as_float = -folded.as_float;
        } else {
            // Negating on the unsigned width wraps INT32_MIN instead of
            // overflowing, which is defined and matches what the VM already
            // does for the same value at runtime.
            folded.as_int = (int32_t)(0u - (uint32_t)folded.as_int);
        }

        unsigned int const_index = constpool_add(state->chunk->const_pool, value_from_literal(folded));
        unsigned int rd = codegen_alloc_register(state, node->span);

        chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_LOAD_CONST, rd, const_index));

        return rd;
    }

    bool is_float = node->type->kind == TYPE_FLOAT;

    Constant zero_value = is_float ? (Constant){.as_float = 0.0f} : (Constant){.as_int = 0};
    unsigned int zero_index = constpool_add(state->chunk->const_pool, zero_value);

    unsigned int zero = codegen_alloc_register(state, node->span);
    chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_LOAD_CONST, zero, zero_index));

    unsigned int operand = codegen_expr(state, inner);
    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(is_float ? OP_SUBF : OP_SUBI, rd, zero, operand));

    return rd;
}

// Logical not lowers to a comparison against false rather than earning an
// opcode: a bool is an int slot holding 0 or 1, so '!b' is exactly 'b == 0',
// and the comparison already produces the 0 or 1 the result must be.
//
// The false has to live in a register: a comparison reads both its operands as
// registers, so there is nowhere for the constant to ride along.
static unsigned int codegen_not_expr(CodegenState *state, ASTExpr *node) {
    ASTExpr *inner = node->unary.target;

    // '!true' is a constant, so it folds rather than emitting a load and a
    // comparison.
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

// 'int(x)' and 'float(x)'. A conversion between the two numeric types is one
// instruction; a conversion to the type the operand already has is none, and
// the operand's register is handed straight back.
static unsigned int codegen_cast_expr(CodegenState *state, ASTExpr *node) {
    ASTExpr *operand = node->cast.operand;

    unsigned int src = codegen_expr(state, operand);

    if (node->type->kind == operand->type->kind) {
        return src;
    }

    unsigned int rd = codegen_alloc_register(state, node->span);
    OpCode op = node->type->kind == TYPE_FLOAT ? OP_ITOF : OP_FTOI;

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, src, 0));

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
        return codegen_load_indirect_struct(state, node, node->type, target, slots);
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

// 'a += b': read what the target holds, apply the operator, write the result
// back where it came from.
//
// The target expression is walked exactly once, and everything after works
// from what that walk produced -- a slot for a variable, a FieldTarget for a
// field or a deref. That is what makes '*f() += 1' call f once: expanding this
// to 'a = a + b' in the parser would name the target twice and call it twice.
static void codegen_compound_assign_stmt(CodegenState *state, ASTCompoundAssignStmt *ast) {
    // Only arithmetic reaches here, so the target is an int or a float in a
    // single slot. The struct and pointer paths a plain assignment needs have
    // nothing to do here.
    assert(type_slot_count(ast->target->type) == 1 && "a compound assignment target is a single slot");

    OpCode op_code =
        ast->target->type->kind == TYPE_FLOAT ? bin_op_to_float_op(ast->op) : bin_op_to_int_op(ast->op);

    if (ast->target->kind == EXPR_VARIABLE) {
        unsigned int rd = codegen_expr(state, ast->target);
        unsigned int rhs = codegen_expr(state, ast->value);

        chunk_add_instruction(state->chunk, VM_ENCODE_R(op_code, rd, rd, rhs));
        return;
    }

    // One walk of the target, reused by both the load and the store below.
    FieldTarget target = ast->target->kind == EXPR_FIELD
                             ? codegen_field_base(state, ast->target, true)
                             : (FieldTarget){
                                   .base = codegen_expr(state, ast->target->unary.target),
                                   .offset = 0,
                                   .indirect = true,
                               };

    size_t size = ast->target->type->size;

    bool load_ok;
    OpCode load_op = field_opcode_for(size, true, target.indirect, &load_ok);

    if (!codegen_field_access_fits(state, ast->target, load_ok, target.offset)) {
        return;
    }

    unsigned int value = codegen_alloc_register(state, ast->target->span);
    chunk_add_instruction(state->chunk,
                          VM_ENCODE_R(load_op, value, target.base, (unsigned int)target.offset));

    unsigned int rhs = codegen_expr(state, ast->value);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op_code, value, value, rhs));

    bool store_ok;
    OpCode store_op = field_opcode_for(size, false, target.indirect, &store_ok);

    if (!codegen_field_access_fits(state, ast->target, store_ok, target.offset)) {
        return;
    }

    chunk_add_instruction(state->chunk,
                          VM_ENCODE_R(store_op, target.base, value, (unsigned int)target.offset));
}

static void codegen_copy_slots(CodegenState *state, unsigned int dest, unsigned int src, unsigned int count) {
    if (dest == src) {
        return;
    }

    // One instruction for the whole run: a struct or a pointer is several
    // slots, and a dispatch per slot is the interpreter's real cost, not the
    // four bytes it moves. A single slot keeps OP_MOVE, which is every scalar
    // and needs no third operand.
    //
    // A run too wide for the 8-bit count field falls back to the loop rather
    // than truncating, so correctness never depends on the encoding's reach.
    if (count > 1 && count <= VM_MAX_MOVE_SLOTS) {
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE_N, dest, src, count));
        return;
    }

    for (unsigned int i = 0; i < count; i++) {
        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_MOVE, dest + i, src + i, 0));
    }
}

// Every variable is a frame local now, including a top-level one: it lives in
// frame zero. The symbol already names its slot, so a read is free.
static unsigned int codegen_variable_expr(CodegenState *state, ASTExpr *node) {
    return codegen_slot_of(state, node->symbol);
}

// Whether a right-hand operand can ride in the instruction's r2 field instead
// of being loaded into a register first.
//
// Only non-negative integer literals in range: r2 is eight unsigned bits, and
// widening it to hold a sign or a float would cost the field it shares with the
// register form. Everything else takes the register path unchanged, so this is
// an optimisation on the common shape rather than a restriction on the language.
static bool codegen_immediate_operand(const ASTExpr *node, unsigned int *out) {
    if (!node || node->kind != EXPR_LITERAL || node->lit.kind != TYPE_INT) {
        return false;
    }

    if (node->lit.as_int < 0 || node->lit.as_int > VM_MAX_IMMEDIATE) {
        return false;
    }

    *out = (unsigned int)node->lit.as_int;

    return true;
}

// The right operand of a binary op: either a register holding it, or the value
// itself when it fits the instruction. Reports which through 'immediate', so the
// caller knows whether to set the k bit.
//
// Generating it is what may allocate a register, so this runs before the result
// register is allocated — the order the original codegen used, and the one the
// register numbering in the tests reflects.
static unsigned int codegen_bin_op_rhs(CodegenState *state, ASTExpr *node, bool *immediate) {
    unsigned int value = 0;

    // Floats have no immediate form: vm_operand2i reads the field as an
    // integer, and a float literal has no eight-bit encoding.
    if (node->bin_op.left->type->kind != TYPE_FLOAT &&
        codegen_immediate_operand(node->bin_op.right, &value)) {
        *immediate = true;
        return value;
    }

    *immediate = false;

    return codegen_expr(state, node->bin_op.right);
}

// Emits one arithmetic or comparison instruction. Shared by both binary-op
// paths so the two cannot disagree about when the k bit is set.
static void codegen_emit_bin_op(CodegenState *state, ASTExpr *node, unsigned int dest, unsigned int lhs,
                                unsigned int rhs, bool immediate) {
    OpCode op_code = node->bin_op.left->type->kind == TYPE_FLOAT ? bin_op_to_float_op(node->bin_op.op)
                                                                 : bin_op_to_int_op(node->bin_op.op);

    chunk_add_instruction(state->chunk, VM_ENCODE_RK(op_code, dest, lhs, rhs, immediate ? 1 : 0));
}

// Emits a binary op into a caller-chosen register rather than a fresh one.
//
// 'x = x + 1' otherwise computes into a temporary and then moves it, because
// the expression allocates its own result and the assignment copies. Writing
// straight into the target drops that move — a third of the instructions in
// the commonest statement there is.
//
// Only for the arithmetic and comparison ops: the logical ones short-circuit
// through a jump and own their result register, so they keep their own path.
//
// The destination is written last, after both operands have been read, so
// naming an operand as the destination is safe: 'x = x + 1' reads x, reads the
// constant, then writes x.
static unsigned int codegen_bin_op_into(CodegenState *state, ASTExpr *node, unsigned int dest) {
    unsigned int lhs = codegen_expr(state, node->bin_op.left);

    bool immediate = false;
    unsigned int rhs = codegen_bin_op_rhs(state, node, &immediate);

    codegen_emit_bin_op(state, node, dest, lhs, rhs, immediate);

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

    bool immediate = false;
    unsigned int rhs = codegen_bin_op_rhs(state, node, &immediate);

    unsigned int result = codegen_alloc_register(state, node->span);

    codegen_emit_bin_op(state, node, result, lhs, rhs, immediate);

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

    if (count > VM_MAX_FRAME_SLOTS || state->next_reg > VM_MAX_FRAME_SLOTS - count) {
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

static bool type_is_owned(const Type *type) { return type_is_pointer(type); }

// Whether an expression hands its caller a reference to own, or merely lends
// one it keeps. 'new' and a call returning '*T' produce a fresh reference the
// receiver is responsible for; reading a variable or a field does not, since
// the variable or the object still holds it.
//
// Derived from the expression rather than recorded on it: the two producing
// forms are exactly these, and a flag on every node would have to be kept in
// step with them for no extra information.
static bool expr_yields_owned(const ASTExpr *expr) {
    if (!expr || !type_is_owned(expr->type)) {
        return false;
    }

    switch (expr->kind) {
    case EXPR_NEW:
    case EXPR_CALL:
        return true;
    default:
        return false;
    }
}

static void codegen_own_slot(CodegenState *state, unsigned int slot, const Type *type) {
    // A 'ref T' slot borrows, so it never owns and is never freed. Callers
    // check this too, but a stray own here would be a use-after-free rather
    // than a leak, so it is refused at the one place ownership is recorded.
    assert(!(type && type->is_ref) && "a 'ref T' slot never owns what it names");

    owned_list_add(&state->owned, (OwnedSlot){.slot = slot, .depth = state->depth, .type = type});

    // Also recorded on the frame, for the unwinder. A failure jumps past every
    // free emitted below, so the frame has to know what it may still be
    // holding — and this is the only place that becomes true.
    //
    // Recorded once per slot: a slot reused by a later block is the same slot,
    // and the runtime clears one when it frees it, so a second entry would only
    // make the unwinder visit an empty slot twice.
    for (size_t i = 0; i < state->frame_refs.size; i++) {
        if (state->frame_refs.data[i].slot == slot) {
            return;
        }
    }

    frame_ref_list_add(&state->frame_refs, (FrameRef){.slot = slot});
}

// Whether this slot already owns a reference, and so has one to drop when it is
// overwritten. A slot merely borrowing — an alias of another variable — has
// nothing to release, and releasing it would free what its owner still holds.
static bool codegen_slot_is_owned(const CodegenState *state, unsigned int slot) {
    for (size_t i = 0; i < state->owned.size; i++) {
        if (state->owned.data[i].slot == slot) {
            return true;
        }
    }

    return false;
}

static void codegen_release_owned(CodegenState *state, unsigned int keep_depth, unsigned int moved) {
    // Innermost first, so an object released here cannot be reached through
    // one released later in the same sweep.
    while (state->owned.size > 0 && state->owned.data[state->owned.size - 1].depth > keep_depth) {
        OwnedSlot owned = state->owned.data[--state->owned.size];

        // A moved slot's reference belongs to whoever it was handed to.
        if (owned.slot == moved) {
            continue;
        }

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_RELEASE, owned.slot, 0, 0));
    }
}

CodegenLabel codegen_create_label(CodegenState *state) {
    return (CodegenLabel){.position = chunk_add_instruction(state->chunk, 0)};
}

void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg) {
    Instruction patch = VM_ENCODE_I(op, reg, state->chunk->instructions.size - label.position - 1);
    chunk_patch_instruction(state->chunk, label.position, patch);
}
