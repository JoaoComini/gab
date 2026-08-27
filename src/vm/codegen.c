#include "codegen.h"

#include "ast/ast.h"
#include "ast/expr.h"
#include "ast/stmt.h"
#include "scope.h"
#include "type.h"
#include "vm/chunk.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include <assert.h>

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

// The frame slot a name was given, and the depth of the block that declared
// it. The depth is what an ownership record has to use: a value assigned to
// the variable inside a nested block belongs to the variable, not to the block
// doing the assigning, so it must not be freed where that block closes.
typedef struct {
    unsigned int slot;
    unsigned int depth;
} SlotBinding;

GAB_HASH_MAP(SlotMap, slot_map, Symbol *, SlotBinding)

#define SLOT_MAP_INITIAL_CAPACITY 16

// A function this unit declared, and the index it was given within the unit.
// The symbol's own func_index cannot hold it: that field is absolute, and a
// call to a function an earlier unit compiled reads it expecting an index this
// unit must not rebase. The two are told apart by which of them has the answer.
#define proto_map_hash(key) (size_t)key
#define proto_map_key_equals(key, other) key == other
#define proto_map_key_dup(key) key
#define proto_map_entry_free(key, value)

GAB_HASH_MAP(ProtoMap, proto_map, Symbol *, size_t)

// A slot holding an owned reference, and the block depth that declared it.
// Kept as a stack because blocks nest and close in order.
typedef struct {
    unsigned int slot;
    unsigned int depth;

    // What the slot holds. Kept for the assert in codegen_own_slot and for
    // reading a chunk back; freeing is one opcode whatever the type, and which
    // dropper runs is read off the allocation's own header at the free.
    TypeHandle type;
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

    // Shared by every state a compile makes, nested bodies included: what the
    // unit accumulates belongs to the compile, not to one function's frame.
    Unit *unit;
    Arena *arena;

    // Where a string literal's characters are interned. They outlive every
    // frame, so a literal's header borrows them.
    StringPool *strings;

    // Shared with every nested state, like the unit itself: a body may call a
    // function declared anywhere in the unit.
    ProtoMap *local_protos;

    // Frame-local, so it is per function body: a nested function generates
    // against its own, and the outer one's slots are not visible in it.
    SlotMap *slots;

    // Slots holding an owned 'box T', innermost block last. Released where the
    // block that declared them closes, rather than where the frame pops:
    // sibling blocks reuse slots, so a per-frame table could say a slot is
    // sometimes a pointer but never when, and a pop after the reuse would
    // release whatever replaced it. Releasing at the close happens while the
    // slot still holds what it was declared as.
    OwnedList owned;

    // Slots holding an owned reference the expression under way produced and no
    // slot will name. Released where the statement ends: an operand reaching into
    // such an object -- a field read from 'new T' -- must keep it alive until the
    // expression is done with it, and nothing later can name it to free it.
    OwnedList temporaries;

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
// A resolved field chain: a base slot plus a byte offset within it. When
// 'indirect' is set the base slot pair holds an address rather than the struct
// itself, which is what a deref in the chain produces.
typedef struct {
    unsigned int base;
    size_t offset;
    bool indirect;
} FieldTarget;

// What the right operand of an arithmetic instruction turned out to be.
typedef enum {
    // A register holding it, which every shape falls back to.
    RHS_REGISTER,

    // A small non-negative integer, encoded in the instruction itself.
    RHS_IMMEDIATE,

    // An index into the constant pool, for a float literal -- which has no
    // eight-bit encoding and would otherwise cost a load of its own.
    RHS_CONSTANT,
} RhsKind;

// Statements, in the order codegen_stmt dispatches them.
static void codegen_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_return_stmt(CodegenState *state, ASTReturnStmt *ast);
static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast);
static bool codegen_expr_into(CodegenState *state, ASTExpr *value, unsigned int dest);
static void codegen_assign_stmt(CodegenState *state, ASTAssignStmt *ast);
static void codegen_compound_assign_stmt(CodegenState *state, ASTCompoundAssignStmt *ast);
static void codegen_block_stmt(CodegenState *state, ASTBlockStmt *ast);
static bool stmt_may_assign(const ASTStmt *stmt, const Symbol *symbol);
static bool for_is_countable(const ASTForStmt *ast, const Symbol **counter, const Symbol **bound);
static void codegen_for_stmt(CodegenState *state, ASTForStmt *ast);
static void codegen_jump_stmt(CodegenState *state, ASTStmt *ast);
static void codegen_if_stmt(CodegenState *state, ASTIfStmt *ast);
static void codegen_reserve_proto(CodegenState *state, ASTFuncDecl *ast);
static void codegen_func_decl_stmt(CodegenState *state, ASTStmt *stmt);

// Expressions, in the order codegen_expr dispatches them.
static unsigned int codegen_expr(CodegenState *state, ASTExpr *ast);
static Constant value_from_literal(Literal lit);
static unsigned int codegen_literal_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_variable_expr(CodegenState *state, ASTExpr *node);
static void codegen_emit_call(CodegenState *state, unsigned int dest, const Symbol *callee, Span span);
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

// Field and pointer access, against a struct in registers or through a pointer.
static FieldTarget codegen_resolve_field_target(CodegenState *state, ASTExpr *node, bool auto_deref);
static bool codegen_field_access_fits(CodegenState *state, ASTExpr *node, bool ok, size_t offset);
static unsigned int field_target_slot_count(FieldTarget target);
static unsigned int codegen_load_indirect_struct(CodegenState *state, ASTExpr *node, TypeHandle type,
                                                 FieldTarget target, unsigned int slots);
static void codegen_store_indirect(CodegenState *state, ASTExpr *node, FieldTarget target, unsigned int src,
                                   unsigned int slots);
static void codegen_store_field(CodegenState *state, ASTExpr *node, unsigned int src);
static void codegen_store_deref(CodegenState *state, ASTExpr *node, unsigned int src);
static void codegen_store_index(CodegenState *state, ASTExpr *node, unsigned int src);
static void codegen_addr_of_into(CodegenState *state, ASTExpr *inner, unsigned int rd, Span span);

// Binary operators: which instruction an operator calls for, and how its right
// operand is encoded.
static bool expr_is_immediate_operand(const ASTExpr *node, unsigned int *out);
static unsigned int codegen_rhs(CodegenState *state, BinOp op, ASTExpr *rhs, TypeHandle left_type,
                                RhsKind *kind);
static OpCode bin_op_opcode_for(BinOp op, TypeHandle left_type, RhsKind kind);
static OpCode bin_op_to_float_op(BinOp bin_op);
static OpCode bin_op_to_int_op(BinOp bin_op);
static void codegen_emit_bin_op(CodegenState *state, ASTExpr *node, unsigned int dest, unsigned int lhs,
                                unsigned int rhs, RhsKind kind);
static unsigned int codegen_bin_op_into(CodegenState *state, ASTExpr *node, unsigned int dest);
static unsigned int codegen_bin_op_expr(CodegenState *state, ASTExpr *node);
static unsigned int codegen_bin_op_logical_expr(CodegenState *state, ASTExpr *node);

// Forward jumps, patched once their target is known.
static CodegenLabel codegen_create_label(CodegenState *state);
static void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg);
static void codegen_emit_loop(CodegenState *state, size_t target);

// Reference ownership. Whether a value carries a reference, which slots hold
// one, and where the releases are emitted.

// Whether a value of this type carries a reference that has to be released. A
// scalar never does, which is why an int costs nothing at runtime: the question
// is settled at compile time from the static type.
// Whether an expression hands its caller a reference to own, or merely lends
// one it keeps.
static bool expr_yields_owned(const ASTExpr *expr);

// Records that 'slot' owns an object for as long as the current block runs.
static void codegen_own_slot(CodegenState *state, unsigned int slot, TypeHandle type);
static void codegen_own_slot_at(CodegenState *state, unsigned int slot, TypeHandle type, unsigned int depth);
static void codegen_record_frame_ref(CodegenState *state, unsigned int slot, TypeHandle type);

// Whether this slot already owns a reference, and so has one to drop when it is
// overwritten.
static bool codegen_slot_is_owned(const CodegenState *state, unsigned int slot);

// Emits a release for every owned slot the current block declared, and drops
// them. 'moved' is a slot whose ownership is leaving the frame — a returned
// pointer — and is skipped; pass VM_INVALID_REGISTER when nothing is moving.
static void codegen_release_owned(CodegenState *state, unsigned int keep_depth, unsigned int moved);

// Forgets that a slot owns, without emitting a release: for 'move', whose
// destination now holds the reference.
static void codegen_disown_slot(CodegenState *state, unsigned int slot);

// Forgets a temporary whose value has been bound to a slot that now owns it.
// Without this the statement's end would release the register the value was
// copied out of, freeing what the binding is holding.
static void codegen_drop_temporary(CodegenState *state, unsigned int slot);

// Emits a release for every owned slot deeper than keep_depth without dropping
// the entries, for a jump that leaves those blocks early.
static void codegen_emit_releases_below(CodegenState *state, unsigned int keep_depth);

// Emits one release, by the type of what the slot holds.
static void codegen_emit_release(CodegenState *state, unsigned int slot, TypeHandle type);

// Frame slots and registers.
static unsigned int codegen_slot_of(CodegenState *state, Symbol *symbol);
static unsigned int codegen_decl_depth_of(CodegenState *state, Symbol *symbol);
static void codegen_set_slot(CodegenState *state, Symbol *symbol, unsigned int slot);
static unsigned int codegen_alloc_register(CodegenState *state, Span span);
static unsigned int codegen_alloc_slots(CodegenState *state, unsigned int count, unsigned int align_slots,
                                        Span span);
static void codegen_release_registers(CodegenState *state, unsigned int saved);
static void codegen_copy_slots(CodegenState *state, unsigned int dest, unsigned int src, unsigned int count);

// Type layout in frame slots.
static unsigned int type_slot_count(TypeHandle type);
static unsigned int type_align_slots(TypeHandle type);
static bool type_is_struct(TypeHandle type);

// Whether declaring a value of this type is what records its ownership, rather
// than the assignment that fills it.
static bool type_owns_through_members(TypeHandle type);

static bool type_moves_as_slots(TypeHandle type);
static OpCode field_opcode_for(size_t size, bool load, bool indirect, bool *ok);

// ---- Generating a unit ----

Unit *codegen_generate(ASTUnit *ast, Arena *arena, StringPool *strings, Diagnostics *diagnostics) {
    Unit *unit = calloc(1, sizeof(Unit));

    if (!unit) {
        return NULL;
    }

    unit->prototypes = func_proto_list_create();
    unit->extern_protos = extern_proto_list_create();
    unit->types = type_list_create();
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

    // Prototype indices first, bodies second — mirroring the resolver, which
    // hoists declarations for the same reason. A body may call a function
    // declared below it, and the OP_CALL it emits needs that function's index
    // before its body has been reached.
    for (size_t i = 0; i < ast->statements.size; i++) {
        ASTStmt *stmt = ast->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL) {
            codegen_reserve_proto(&state, &stmt->func_decl);
        }
    }

    for (size_t i = 0; i < ast->statements.size; i++) {
        codegen_stmt(&state, ast->statements.data[i]);
    }

    // The top level ends in a return like any other body, so every chunk ends by
    // saying so rather than by the interpreter noticing it has run out of
    // instructions. That is what lets a straight-line step skip the bounds
    // check: the last instruction it can reach is a return, which leaves through
    // the frame path instead.
    OpCode last = state.chunk->instructions.size > 0
                      ? VM_DECODE_OPCODE(instruction_list_back(&state.chunk->instructions))
                      : OP_LOAD_CONST;

    if (state.chunk->instructions.size == 0 || (last != OP_RETURN && last != OP_RETURN_N)) {
        chunk_add_instruction(state.chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));
    }

    // The slots were only ever true of this compile, so they go with it.
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

// ---- Statements ----

// Frees every unbound owned value the statement produced. Emitted at the end of
// the statement rather than where each was created: an operand may still be
// reaching into the object while the rest of the expression is evaluated.
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

        // 'new Player;' on its own line owns a reference nothing will ever
        // store, so the statement is where it dies. Without this the object
        // leaks with no name left to release it by.
        if (expr_yields_owned(ast->expr.value)) {
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
        // Types are resolved at compile time and emit no code.
        break;
    }

    codegen_release_temporaries(state);

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

    // Ownership of the result passes to the caller, so a value computed into a
    // temporary here is no longer this frame's to free.
    codegen_drop_temporary(state, reg);

    // A temporary is the innermost thing alive, so it goes first. The returned
    // value has already been copied into its own slot, so nothing released here
    // is what is being returned.
    codegen_release_temporaries(state);

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

// What codegen_walk_owning_slots does at each owning pointer it finds.
typedef enum {
    // Write null, so a slot that has never been stored to is safe to free.
    OWNING_SLOT_NULL,

    // Record it, so the block that declared it frees it.
    OWNING_SLOT_OWN,

    // Forget it, for 'move': the destination holds the reference now.
    OWNING_SLOT_DISOWN,
} OwningSlotAction;

// Visits every owning pointer a value of this type holds at 'base' -- itself if
// it is one, and otherwise each of its fields, recursing through nested structs
// whose slots are laid out inline.
//
// A struct owns through its fields rather than as a slot, so nulling, owning
// and disowning one all mean doing the same walk and acting at each pointer.
// A 'ref' field is skipped throughout: nothing frees it, so nothing reads it as
// an owner. A string answers here as a struct does, through the field naming
// its characters.
static void codegen_walk_owning_slots(CodegenState *state, TypeHandle type, unsigned int base,
                                      OwningSlotAction action) {
    if (type_is_indirect(type)) {
        if (!type_is_owned(type)) {
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

    // A string header owns as one value rather than through its 'data' field,
    // which is a raw address claiming nothing: what must be freed is the block,
    // and the count saying how far to walk it sits in the slot beside that
    // pointer. Descending into the field would hand the free path the pointer
    // alone.
    //
    // An array is here for the same reason and a different one: it owns through
    // elements its type says how to walk, so one release naming it frees them
    // all -- the way one release naming a struct frees every field that owns.
    if (type && (type->kind == TYPE_ARRAY || type->kind == TYPE_STRING)) {
        if (!type_is_owned(type)) {
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

    // A struct owns through its fields: the walk reaches what each names by that
    // field's offset rather than by knowing where its kind keeps things.
    if (!type || type_field_count(type) == 0) {
        return;
    }

    for (size_t i = 0; i < type_field_count(type); i++) {
        const TypeField *field = &type_fields(type)[i];

        codegen_walk_owning_slots(state, field->type, base + (unsigned int)(field->offset / VM_SLOT_SIZE),
                                  action);
    }
}
static void codegen_var_decl_stmt(CodegenState *state, ASTVarDecl *ast) {
    Span span = ast->initializer ? ast->initializer->span : (Span){0};

    codegen_set_slot(state, ast->symbol,
                     codegen_alloc_slots(state, type_slot_count(ast->symbol->var.type),
                                         type_align_slots(ast->symbol->var.type), span));

    // A 'ref T' local borrows: nothing frees it, so its slot is never owned and
    // never listed on the frame. It needs no null-init either — nothing will
    // read it as an owner.
    bool is_ref = ast->symbol->var.type && ast->symbol->var.type->kind == TYPE_REF;

    if (!ast->initializer) {
        // An owning slot holds nothing until something is stored, so the store
        // that initializes it has no previous occupant to free.
        if (!is_ref) {
            unsigned int slot = codegen_slot_of(state, ast->symbol);

            codegen_walk_owning_slots(state, ast->symbol->var.type, slot, OWNING_SLOT_NULL);

            // Owned from here, since a store reaches into the value rather
            // than replacing it and nothing else would record it. A bare
            // pointer takes ownership where it is assigned instead.
            if (type_owns_through_members(ast->symbol->var.type)) {
                codegen_walk_owning_slots(state, ast->symbol->var.type, slot, OWNING_SLOT_OWN);
            }
        }

        return;
    }

    // The variable's own slot is already reserved; everything the initializer
    // allocates above it is a temporary and is reclaimed here.
    unsigned int saved = state->next_reg;

    unsigned int slot = codegen_slot_of(state, ast->symbol);

    // Generated straight into the variable's slot where the shape allows it.
    // An owned initialiser is excluded: the ownership bookkeeping below reads
    // the value's own register to decide what this slot takes over.
    if (!is_ref && !type_is_owned(ast->symbol->var.type) &&
        codegen_expr_into(state, ast->initializer, slot)) {
        codegen_release_registers(state, saved);
        return;
    }

    unsigned int r1 = codegen_expr(state, ast->initializer);

    codegen_copy_slots(state, slot, r1, type_slot_count(ast->symbol->var.type));

    // The variable takes over the initializer's object, so an owned result is
    // now owned by this slot. A borrowed one — reading another variable — is
    // left alone: the slot it came from still owns it, and freeing here would
    // free it twice. A 'ref T' slot never owns, whatever it was given.
    //
    // A struct owns through its fields rather than as one slot, so it is
    // recorded field by field: releasing its base would free an address that is
    // the struct itself rather than anything it holds.
    if (!is_ref && expr_yields_owned(ast->initializer)) {
        codegen_walk_owning_slots(state, ast->symbol->var.type, slot, OWNING_SLOT_OWN);

        // The value was copied out of r1 into this slot, which owns it now.
        codegen_drop_temporary(state, r1);
    }

    codegen_release_registers(state, saved);
}

// Generates 'value' so that it lands in 'dest', rather than wherever it would
// naturally go with a copy afterwards. Returns false when the shape has no
// such form and the caller must fall back to generating and copying.
//
// Only single-slot scalars qualify: a struct is copied as a run, and the
// ownership bookkeeping around an owned pointer wants the value in hand.
static bool codegen_expr_into(CodegenState *state, ASTExpr *value, unsigned int dest) {
    if (type_slot_count(value->type) != 1 || type_is_owned(value->type)) {
        return false;
    }

    switch (value->kind) {
    case EXPR_LITERAL: {
        unsigned int index = constpool_add(state->chunk->const_pool, value_from_literal(value->lit));

        chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_LOAD_CONST, dest, index));
        return true;
    }
    case EXPR_VARIABLE:
        codegen_copy_slots(state, dest, codegen_slot_of(state, value->symbol), 1);
        return true;

    // A binary op already has a form that computes into a destination. The
    // logical pair is excluded because they short-circuit through a jump and
    // own the register they land in.
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
    // Storing into a field, or through a pointer, puts a value somewhere that
    // outlives the statement. An owning target takes over what it is given and
    // frees whatever it held before; a 'ref T' target owns nothing either way,
    // so it falls through to the ordinary store below.
    bool target_owns = (ast->target->kind == EXPR_FIELD || ast->target->kind == EXPR_DEREF ||
                        ast->target->kind == EXPR_INDEX) &&
                       type_is_owned(ast->target->type);

    if (target_owns) {
        // The value stored is one nothing else owns: the resolver refuses a
        // borrow here, naming the move or the duplicate that was meant.
        //
        // Read what the destination holds before overwriting it, and free it
        // after the store rather than before: freeing first would leave a
        // window where the field points at freed memory if anything observed
        // it.
        //
        // Copied into a slot of its own first. A pointer field is addressed
        // rather than loaded -- codegen_field_expr hands back the field's own
        // slot -- so releasing that slot directly would free whatever the store
        // had just written into it. The local-variable path below copies for
        // the same reason.
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

        // As wide as the value being replaced: a string is a header of several
        // slots, and copying only the first would leave the release reading a
        // length that belongs to whatever the store wrote.
        unsigned int width = type_slot_count(ast->target->type);
        unsigned int old =
            codegen_alloc_slots(state, width, type_align_slots(ast->target->type), ast->target->span);

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

        // The field owns what was stored, so the statement's end must not also
        // release the register it was computed in.
        codegen_drop_temporary(state, src);

        codegen_emit_release(state, old, ast->target->type);
        return;
    }

    // A field target is written in place through its base slot, so the target
    // is never materialised as a value first.
    if (ast->target->kind == EXPR_FIELD) {
        codegen_store_field(state, ast->target, codegen_expr(state, ast->value));
        return;
    }

    // Likewise an element, which is written through the address computed for
    // it rather than over the header naming the block.
    if (ast->target->kind == EXPR_INDEX) {
        codegen_store_index(state, ast->target, codegen_expr(state, ast->value));
        return;
    }

    // Likewise a deref: '*p = v' writes through p rather than over it.
    if (ast->target->kind == EXPR_DEREF) {
        codegen_store_deref(state, ast->target, codegen_expr(state, ast->value));
        return;
    }

    unsigned int rd = codegen_expr(state, ast->target);

    // Computed straight into the target where the shape allows it, instead of
    // into a temporary this would then have to copy down.
    if (!type_is_owned(ast->target->type) && codegen_expr_into(state, ast->value, rd)) {
        return;
    }

    unsigned int r1 = codegen_expr(state, ast->value);

    bool target_is_ref = ast->target->type && ast->target->type->kind == TYPE_REF;

    // Reassigning a variable that owns an object: it frees the old one and takes
    // over the new. Freeing after the store, for the same reason a field store
    // does — 'p = p' must not free what it is about to keep.
    //
    // A 'ref T' slot owns nothing, so it is a plain store however it is written.
    if (!target_is_ref && type_is_owned(ast->target->type) && codegen_slot_is_owned(state, rd)) {
        // The value is one nothing else owns: the resolver refuses a borrow
        // here, naming the move or the duplicate that was meant.
        // As wide as what is being replaced: a string is a header of several
        // slots, and saving only the first would release a pointer paired with
        // whatever length the new value wrote.
        unsigned int width = type_slot_count(ast->target->type);
        unsigned int old =
            codegen_alloc_slots(state, width, type_align_slots(ast->target->type), ast->target->span);

        codegen_copy_slots(state, old, rd, width);
        codegen_copy_slots(state, rd, r1, width);

        // The variable owns what was just stored, so the statement's end must
        // not also release the register it was computed in.
        codegen_drop_temporary(state, r1);

        codegen_emit_release(state, old, ast->target->type);
        return;
    }

    codegen_copy_slots(state, rd, r1, type_slot_count(ast->target->type));

    // A borrowed value assigned into a slot that owns nothing yet leaves the
    // slot borrowing too, so there is nothing to record. An owned one makes the
    // slot an owner from here on.
    //
    // Recorded at the depth the variable was declared at, not the depth doing
    // the assigning: 'if c { a = new Box; }' gives the object to a, which
    // outlives the arm, so the arm's close must not free it.
    if (!target_is_ref && type_is_owned(ast->target->type) && expr_yields_owned(ast->value)) {
        Symbol *target = ast->target->symbol;

        codegen_own_slot_at(state, rd, ast->target->type,
                            target ? codegen_decl_depth_of(state, target) : state->depth);
    }
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

    if (ast->target->kind == EXPR_VARIABLE) {
        unsigned int rd = codegen_expr(state, ast->target);

        RhsKind rhs_kind = RHS_REGISTER;
        unsigned int rhs = codegen_rhs(state, ast->op, ast->value, ast->target->type, &rhs_kind);

        OpCode op_code = bin_op_opcode_for(ast->op, ast->target->type, rhs_kind);

        chunk_add_instruction(state->chunk,
                              VM_ENCODE_RK(op_code, rd, rd, rhs, rhs_kind == RHS_IMMEDIATE ? 1 : 0));
        return;
    }

    // One walk of the target, reused by both the load and the store below.
    FieldTarget target = ast->target->kind == EXPR_FIELD
                             ? codegen_resolve_field_target(state, ast->target, true)
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

    // Released before the slots are reclaimed, so each still holds what it was
    // declared as. A block ending in 'return' has already released these on the
    // way out, and the list is empty by now.
    codegen_release_owned(state, enclosing_depth, VM_INVALID_REGISTER);

    state->depth = enclosing_depth;
    codegen_release_registers(state, saved);
}

// Whether a statement can write to 'symbol', looking through every nested
// statement. Conservative in the one direction that matters: an unrecognised
// shape answers yes, so a loop is only fused when nothing in it could have
// touched the counter or the bound.
static bool stmt_may_assign(const ASTStmt *stmt, const Symbol *symbol) {
    if (!stmt) {
        return false;
    }

    switch (stmt->kind) {
    case STMT_ASSIGN:
        return stmt->assign.target->symbol == symbol;
    case STMT_COMPOUND_ASSIGN:
        return stmt->compound_assign.target->symbol == symbol;
    case STMT_VAR_DECL:
        return stmt->var_decl.symbol == symbol;
    case STMT_BLOCK:
        for (size_t i = 0; i < stmt->block.list.size; i++) {
            if (stmt_may_assign(stmt->block.list.data[i], symbol)) {
                return true;
            }
        }

        return false;
    case STMT_IF:
        return stmt_may_assign(stmt->ifstmt.then_block, symbol) ||
               stmt_may_assign(stmt->ifstmt.else_block, symbol);
    case STMT_FOR:
        return stmt_may_assign(stmt->forstmt.init, symbol) || stmt_may_assign(stmt->forstmt.post, symbol) ||
               stmt_may_assign(stmt->forstmt.body, symbol);
    case STMT_EXPR:
    case STMT_FUNC_DECL:
    case STMT_STRUCT_DECL:
    case STMT_JUMP:
    case STMT_RETURN:
        return false;
    }

    return true;
}

// A loop OP_FOR_LOOP can stand for: an int counter compared '<' against
// something, stepped by one, with neither changed anywhere in the body.
//
// Everything here is a fact codegen can check locally. The counter and the
// bound are plain variables, so 'may assign' is a search for their symbol
// rather than an aliasing question -- taking a pointer to either would make
// this unsound, which is why an addressed counter is refused too.
// Whether this literal is the integer one, which is the only step the fused
// instruction takes.
static bool is_one(const ASTExpr *expr) {
    return expr->kind == EXPR_LITERAL && expr->lit.kind == TYPE_INT && expr->lit.as_int == 1;
}

// Whether a statement steps 'counter' by one. Two spellings reach here: the
// compound 'i += 1', and the plain 'i = i + 1' it stands for -- including
// '1 + i', since addition commutes and the two say the same thing.
static bool for_step_is_one(const ASTStmt *post, const Symbol *counter) {
    if (post->kind == STMT_COMPOUND_ASSIGN) {
        const ASTCompoundAssignStmt *step = &post->compound_assign;

        if (step->op != BIN_OP_ADD || step->target->kind != EXPR_VARIABLE ||
            step->target->symbol != counter) {
            return false;
        }

        return is_one(step->value);
    }

    if (post->kind != STMT_ASSIGN) {
        return false;
    }

    const ASTExpr *target = post->assign.target;
    const ASTExpr *value = post->assign.value;

    if (target->kind != EXPR_VARIABLE || target->symbol != counter) {
        return false;
    }

    if (value->kind != EXPR_BIN_OP || value->bin_op.op != BIN_OP_ADD) {
        return false;
    }

    // The counter on one side and the literal one on the other, either way
    // round. Anything else assigns something that is not this counter stepped.
    const ASTExpr *lhs = value->bin_op.left;
    const ASTExpr *rhs = value->bin_op.right;

    if (lhs->kind == EXPR_VARIABLE && lhs->symbol == counter) {
        return is_one(rhs);
    }

    if (rhs->kind == EXPR_VARIABLE && rhs->symbol == counter) {
        return is_one(lhs);
    }

    return false;
}

static bool for_is_countable(const ASTForStmt *ast, const Symbol **counter, const Symbol **bound) {
    if (!ast->condition || !ast->post || !ast->body) {
        return false;
    }

    // 'i < bound', both plain int variables.
    if (ast->condition->kind != EXPR_BIN_OP || ast->condition->bin_op.op != BIN_OP_LESS) {
        return false;
    }

    const ASTExpr *left = ast->condition->bin_op.left;
    const ASTExpr *right = ast->condition->bin_op.right;

    if (left->kind != EXPR_VARIABLE || right->kind != EXPR_VARIABLE) {
        return false;
    }

    if (!left->type || left->type->kind != TYPE_INT || !right->type || right->type->kind != TYPE_INT) {
        return false;
    }

    // The step, on the same variable the condition tests. Both spellings of it:
    // 'i += 1', and the 'i = i + 1' it is shorthand for. They are one operation,
    // so recognising only the shorter one would make the fused instruction
    // depend on how the step was written rather than on what it does.
    if (!for_step_is_one(ast->post, left->symbol)) {
        return false;
    }

    // A pinned variable has had its address taken, so a store through a
    // pointer could change it without naming it, and the search below would
    // not see that.
    if (left->symbol->pinned || right->symbol->pinned) {
        return false;
    }

    // Refused rather than merely reasoned about: the fused instruction still
    // reads both operands afresh, so a body writing to either would run
    // correctly, but it would no longer be the loop this shape describes.
    if (stmt_may_assign(ast->body, left->symbol) || stmt_may_assign(ast->body, right->symbol)) {
        return false;
    }

    *counter = left->symbol;
    *bound = right->symbol;

    return true;
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

    // A counting loop ends in one instruction that steps, tests and jumps back.
    // The entry test stays a separate compare, since it runs once: it is the
    // per-iteration cost the fused form is for.
    const Symbol *counter = NULL;
    const Symbol *bound = NULL;

    if (for_is_countable(ast, &counter, &bound)) {
        unsigned int counter_reg = codegen_slot_of(state, (Symbol *)counter);
        unsigned int bound_reg = codegen_slot_of(state, (Symbol *)bound);

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

        // Too long a body to reach back in eight signed bits. Nothing is
        // emitted yet that assumes otherwise, so the general form still works.
        if (back >= -VM_MAX_LOOP_OFFSET) {
            chunk_add_instruction(state->chunk,
                                  VM_ENCODE_R(OP_FOR_LOOP, counter_reg, bound_reg, (unsigned int)back));

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

// Claims the index a function's call will encode, without generating anything.
// Reserving it before any body is generated is what lets a body call a function
// whose own body has not been reached yet — the recursion case, and now the
// forward-call case the resolver's hoisting admits.
//
// An extern is counted in its own space, since OP_CALL_EXTERN indexes a
// different table: numbering both from one counter would leave each table as
// sparse as the other is full.
//
// The index is the unit's own. What the call finally encodes is this plus the
// base linking assigns to that table, which is why every call emitted against
// it is recorded for relocation.
static void codegen_reserve_proto(CodegenState *state, ASTFuncDecl *ast) {
    if (!ast->symbol || proto_map_lookup(state->local_protos, ast->symbol)) {
        return;
    }

    size_t local;

    if (ast->symbol->func.is_extern) {
        extern_proto_list_add(&state->unit->extern_protos, (ExternProto){0});

        local = state->unit->extern_protos.size - 1;
    } else {
        FuncPrototype *proto = arena_alloc(state->arena, sizeof(FuncPrototype));
        *proto = (FuncPrototype){0};

        func_proto_list_add(&state->unit->prototypes, proto);

        local = state->unit->prototypes.size - 1;
    }

    proto_map_insert(state->local_protos, ast->symbol, local);
    proto_binding_list_add(&state->unit->bindings,
                           (ProtoBinding){.symbol = ast->symbol, .local_index = local});
}

static void codegen_func_decl_stmt(CodegenState *state, ASTStmt *stmt) {
    ASTFuncDecl *ast = &stmt->func_decl;

    // Reserved by the pre-pass for a top-level function; a nested one, which no
    // pre-pass saw, reserves its own here.
    codegen_reserve_proto(state, ast);

    if (!ast->symbol) {
        return;
    }

    const size_t *local = proto_map_lookup(state->local_protos, ast->symbol);

    if (!local) {
        return;
    }

    size_t func_index = *local;

    // An extern emits no code and carries no body yet. Which host function it
    // binds to is a question only a VM can answer, so the unit records the ask
    // and linking either answers all of them or installs nothing.
    if (ast->symbol->func.is_extern) {
        state->unit->extern_protos.data[func_index] = (ExternProto){.symbol = ast->symbol};

        extern_request_list_add(
            &state->unit->externs,
            (ExternRequest){.local_index = func_index, .symbol = ast->symbol, .span = stmt->span});

        return;
    }

    Chunk *func_chunk = chunk_create();

    unsigned int func_next_reg = 1;

    // The body generates against its own frame, so its slots are its own.
    CodegenState func_state = (CodegenState){
        .chunk = func_chunk,
        .unit = state->unit,
        .arena = state->arena,
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

    // Parameters are placed rather than allocated, so codegen_alloc_slots never
    // sees them and its bound does not apply. A signature wide enough to run
    // past the frame has to be caught here: past this point a parameter's slot
    // number is an operand no instruction can encode, and the frame it asks for
    // is one no stack can reserve.
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

    // An owning parameter frees what it was handed when the call ends, so it
    // joins the owned set exactly as a local declared at the top of the body
    // would. Depth 1 is that body: a 'return' releases everything deeper than
    // 0, and the body block's close does the same, so either exit frees it once.
    //
    // Registered after the frame-size check, since a signature that failed it
    // has already torn this state down.
    func_state.depth = 1;

    for (size_t i = 0; i < ast->params.size; i++) {
        Symbol *param = ast->params.data[i]->symbol;

        if (type_is_owned(param->var.type)) {
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
        // The receiver is parameter zero, so it counts.
        .arity = ast->params.size + (ast->receiver ? 1 : 0),
        .max_registers = func_state.max_reg,
        .refs = func_state.frame_refs,
    };

    owned_list_free(&func_state.owned);
    owned_list_free(&func_state.temporaries);
    slot_map_destroy(func_state.slots);
}

// ---- Expressions ----

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
    case EXPR_MOVE: {
        // The value is produced exactly as it would be without the keyword.
        // What the move changes is who frees it: the source slot gives up its
        // reference, so its block must not release it.
        unsigned int reg = codegen_expr(state, ast->unary.target);

        if (ast->unary.target->kind == EXPR_VARIABLE && ast->unary.target->symbol) {
            codegen_walk_owning_slots(state, ast->unary.target->type,
                                      codegen_slot_of(state, ast->unary.target->symbol), OWNING_SLOT_DISOWN);
        }

        return reg;
    }
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

static unsigned int codegen_string_literal(CodegenState *state, ASTExpr *node) {
    // Decoded and interned by the lexer, so the node already holds the String *.
    String *text = node->lit.as_string;

    unsigned int rd = codegen_alloc_slots(state, VM_STRING_SLOTS, VM_INDIRECT_SLOTS, node->span);

    // Interned within the unit, as a type index is: the index means nothing
    // until linking gives the unit its base. See codegen_new_expr.
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
    if (node->lit.kind == TYPE_STRING) {
        return codegen_string_literal(state, node);
    }

    unsigned int const_index = constpool_add(state->chunk->const_pool, value_from_literal(node->lit));
    unsigned int r1 = codegen_alloc_register(state, node->span);
    Instruction load_const = VM_ENCODE_I(OP_LOAD_CONST, r1, const_index);

    chunk_add_instruction(state->chunk, load_const);

    return r1;
}

// Every variable is a frame local now, including a top-level one: it lives in
// frame zero. The symbol already names its slot, so a read is free.
static unsigned int codegen_variable_expr(CodegenState *state, ASTExpr *node) {
    return codegen_slot_of(state, node->symbol);
}

// The call instruction itself, shared by a plain call and a method call: by
// this point the two have laid out an identical block and differ in nothing the
// instruction records.
//
// Which opcode is which table the callee lives in, and that is the declaration's
// own 'is_extern' rather than anything this has to work out: a C body is reached
// by OP_CALL_EXTERN and a bytecode one by OP_CALL.
//
// I-type, so the index gets the 17-bit field. It is not a register, and while it
// rode in an 8-bit one a single VM could hold only 255 functions across every
// module it ever loaded. No argument count is encoded: the callee's frame is
// based at dest, so the arguments written above dest already are its parameters,
// and its size comes from the prototype.
static void codegen_emit_call(CodegenState *state, unsigned int dest, const Symbol *callee, Span span) {
    // A function this unit declared is numbered by the unit and rebased at link;
    // one an earlier unit declared already has its final index and must be left
    // alone. Which it is, is which of the two knows the answer.
    const size_t *local = proto_map_lookup(state->local_protos, (Symbol *)callee);
    size_t index = local ? *local : callee->func.func_index;

    if (index == SYMBOL_FUNC_NO_BODY) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "call to a function with no body");
        }

        state->failed = true;
        return;
    }

    bool is_extern = callee->func.is_extern;

    // Only an absolute index can be bounds-checked here. A unit-local one is
    // checked at link, where the base it will be given is known.
    if (!local && index > (is_extern ? VM_MAX_EXTERN_PROTOS : VM_MAX_PROTOTYPES)) {
        if (!state->failed) {
            diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "too many functions in one program");
        }

        state->failed = true;
        return;
    }

    size_t offset = chunk_add_instruction(
        state->chunk, VM_ENCODE_I(is_extern ? OP_CALL_EXTERN : OP_CALL, dest, (unsigned int)index));

    if (local) {
        relocation_list_add(is_extern ? &state->unit->extern_relocations : &state->unit->proto_relocations,
                            (Relocation){.chunk = state->chunk, .offset = offset});
    }
}

// Whether the callee's parameter in this position owns what it is given, and
// so frees it itself. The resolver folds a method's receiver into the argument
// list as parameter zero, so the positions line up without adjustment.
static bool param_owns(const Symbol *callee, size_t index) {
    if (!callee || callee->kind != SYMBOL_FUNC || index >= callee->func.param_count) {
        return false;
    }

    TypeHandle param = callee->func.params[index];

    return type_is_owned(param);
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
    // once the call returns. A 'ref' parameter borrows, so the callee frees
    // nothing and an owned temporary would otherwise belong to nobody the
    // moment the argument block is reclaimed.
    //
    // An owning parameter is the other case: the callee took the reference and
    // frees it itself, so releasing here would free it twice.
    //
    // Bounded by the frame rather than by the argument count: the block was
    // reserved above, and codegen_alloc_slots refuses one wider than a frame,
    // so there can never be more owned arguments than slots to hold them.
    // The slot each owned argument was left in, with what it holds: a release
    // is by type, and the argument's own is what the copy above put there.
    struct {
        unsigned int slot;
        TypeHandle type;
    } owned_args[VM_MAX_FRAME_SLOTS];
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
        if (expr_yields_owned(arg) && !param_owns(node->symbol, i)) {
            assert(owned_arg_count < VM_MAX_FRAME_SLOTS && "more owned arguments than a frame has slots");

            // The callee's frame is based at dest, so its return writes over
            // the low end of the argument block. An argument the return
            // reaches has to survive somewhere else to be released after the
            // call; one it does not is released where it was passed.
            unsigned int owner = dest + offset;

            if (offset < return_slots) {
                owner = codegen_alloc_slots(state, slots, 1, arg->span);

                codegen_copy_slots(state, owner, arg_reg, slots);
            }

            owned_args[owned_arg_count].slot = owner;
            owned_args[owned_arg_count].type = arg->type;
            owned_arg_count++;

            // That slot is the only owner now: an expression that
            // registered its result as a statement temporary -- a join does --
            // would otherwise be freed both here and where the statement ends.
            codegen_drop_temporary(state, arg_reg);
        }

        offset += slots;
    }

    codegen_emit_call(state, dest, node->symbol, node->span);

    // After the call, so the callee still has its arguments, and before the
    // registers are reclaimed, so the slots still hold what was put in them.
    for (size_t i = 0; i < owned_arg_count; i++) {
        codegen_emit_release(state, owned_args[i].slot, owned_args[i].type);
    }

    codegen_release_registers(state, saved);

    return dest;
}

// Writes 'count' times 'stride' into 'dest'. A stride wider than an operand
// takes the register form, which costs a load of the width rather than refusing
// the element.
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

// The address of the array itself, whichever way the target names it. The
// elements are the array, so this is where they begin -- a local names its own
// slots, and a target behind a pointer is that pointer.
static unsigned int codegen_index_base(CodegenState *state, ASTExpr *target) {
    if (!type_is_indirect(target->type)) {
        unsigned int base = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, target->span);

        codegen_addr_of_into(state, target, base, target->span);

        return base;
    }

    unsigned int pointer = codegen_expr(state, target);

    // Every level, as a field access reaches through every level: a
    // 'ref box Array int,3' is two hops from the elements.
    TypeHandle type = target->type;

    while (type_is_indirect(type_pointee(type))) {
        unsigned int next = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, target->span);

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_LOAD_PTR_N, next, pointer, VM_INDIRECT_SLOTS));

        pointer = next;
        type = type_pointee(type);
    }

    return pointer;
}

// The address of 'xs[i]', bounds-checked, left in a fresh pointer pair. The
// element is at 'base + i * stride', so this is the one access whose offset is
// computed rather than folded into the instruction -- which is why it lands in
// an indirect FieldTarget and every read and write of it goes through the same
// paths a field reached through a pointer does.
static unsigned int codegen_index_address(CodegenState *state, ASTExpr *node) {
    TypeHandle array_type = node->index.array_type;
    TypeHandle element = type_array_element(array_type);

    unsigned int base = codegen_index_base(state, node->index.target);
    unsigned int index = codegen_expr(state, node->index.index);

    // The length is the type's, so the bound is an immediate rather than
    // something read from the value being indexed.
    chunk_add_instruction(state->chunk, VM_ENCODE_RK(OP_BOUNDS_CHECK, base, index,
                                                     (unsigned int)type_array_length(array_type), 1));

    unsigned int offset = codegen_alloc_register(state, node->span);

    codegen_scale_by_stride(state, offset, index, element->size, node->span);

    unsigned int address = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_ADD_PTR_REG, address, base, offset));

    return address;
}

// '[a, b, c]' fills the array's own slots, in order. The elements are the
// array, so each is generated straight into where it lands rather than into a
// temporary the whole run is then copied from.
static unsigned int codegen_array_lit_expr(CodegenState *state, ASTExpr *node) {
    TypeHandle type = node->type;
    TypeHandle element = type_array_element(type);

    unsigned int rd = codegen_alloc_slots(state, type_slot_count(type), type_align_slots(type), node->span);

    unsigned int element_slots = type_slot_count(element);

    for (size_t i = 0; i < node->array_lit.elements.size; i++) {
        ASTExpr *value = node->array_lit.elements.data[i];

        // Each element sits a whole element's width along, which is the stride
        // an index computes too.
        unsigned int slot = rd + (unsigned int)i * element_slots;

        if (!codegen_expr_into(state, value, slot)) {
            codegen_copy_slots(state, slot, codegen_expr(state, value), element_slots);
        }

        codegen_own_slot(state, slot, element);
    }

    return rd;
}

// The FieldTarget an element access reads and writes through: always indirect,
// since the address was computed rather than named.
static FieldTarget codegen_index_target(CodegenState *state, ASTExpr *node) {
    return (FieldTarget){.base = codegen_index_address(state, node), .offset = 0, .indirect = true};
}

static unsigned int codegen_index_expr(CodegenState *state, ASTExpr *node) {
    FieldTarget target = codegen_index_target(state, node);

    if (type_moves_as_slots(node->type)) {
        return codegen_load_indirect_struct(state, node, node->type, target, type_slot_count(node->type));
    }

    bool ok;
    OpCode op = field_opcode_for(node->type->size, true, true, &ok);

    if (!codegen_field_access_fits(state, node, ok, target.offset)) {
        return 0;
    }

    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, rd, target.base, 0));

    return rd;
}

// Writes a value into 'xs[i]', through the address computed for the element.
static void codegen_store_index(CodegenState *state, ASTExpr *node, unsigned int src) {
    FieldTarget target = codegen_index_target(state, node);

    if (type_moves_as_slots(node->type)) {
        codegen_store_indirect(state, node, target, src, type_slot_count(node->type));
        return;
    }

    bool ok;
    OpCode op = field_opcode_for(node->type->size, false, true, &ok);

    if (!codegen_field_access_fits(state, node, ok, target.offset)) {
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, target.base, src, 0));
}

static unsigned int codegen_field_expr(CodegenState *state, ASTExpr *node) {
    FieldTarget target = codegen_resolve_field_target(state, node, true);

    // A multi-slot field is addressed, not loaded: its slots are already laid
    // out inline, so the caller reads them where they sit. Through a pointer
    // there are no such slots, so it is copied out instead.
    if (type_moves_as_slots(node->type)) {
        if (target.indirect) {
            return codegen_load_indirect_struct(state, node, node->type, target, type_slot_count(node->type));
        }

        return field_target_slot_count(target);
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

static unsigned int codegen_addr_of_expr(CodegenState *state, ASTExpr *node) {
    ASTExpr *inner = node->unary.target;

    // 'ref *p' is just p: the deref would only load what the address already is.
    if (inner->kind == EXPR_DEREF) {
        return codegen_expr(state, inner->unary.target);
    }

    // 'ref p' where p is a pointer names p itself, so the chain must stop at it
    // rather than reach through.
    FieldTarget target = codegen_resolve_field_target(state, inner, false);

    unsigned int rd = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);

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

// Negation is its own opcode rather than a subtraction from zero. The zero
// would have to live in a register -- only the second operand of an arithmetic
// instruction may be immediate, and the zero there is the first -- so lowering
// it that way spends a load and a register on every execution.
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

    unsigned int operand = codegen_expr(state, inner);
    unsigned int rd = codegen_alloc_register(state, node->span);

    chunk_add_instruction(state->chunk, VM_ENCODE_R(is_float ? OP_NEGF : OP_NEGI, rd, operand, 0));

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

// Interns a type within the unit and returns its index. An index means nothing
// until linking gives the unit its base, so a type an earlier unit registered is
// registered again here and the two are reconciled at link.
static unsigned int codegen_type_index(CodegenState *state, TypeHandle type) {
    for (size_t i = 0; i < state->unit->types.size; i++) {
        if (state->unit->types.data[i] == type) {
            return (unsigned int)i;
        }
    }

    type_list_add(&state->unit->types, (TypeHandle)type);

    return (unsigned int)(state->unit->types.size - 1);
}

// Emits the release of a slot holding a value of this type, and the relocation
// its type index needs. Every free of a named slot goes through here, so what a
// release frees is always the type codegen knew rather than whatever the object
// turns out to say.
static void codegen_emit_release(CodegenState *state, unsigned int slot, TypeHandle type) {
    size_t offset =
        chunk_add_instruction(state->chunk, VM_ENCODE_I(OP_RELEASE, slot, codegen_type_index(state, type)));

    relocation_list_add(&state->unit->type_relocations,
                        (Relocation){.chunk = state->chunk, .offset = offset});
}

// 'new T' allocates and leaves an owned 'box T' in a pointer-sized destination.
// The type travels by index because a type is 8 bytes and cannot ride in an
// instruction; the list is interned by pointer identity, which the type system
// already guarantees.

static unsigned int codegen_new_expr(CodegenState *state, ASTExpr *node) {
    unsigned int rd = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);

    size_t offset = chunk_add_instruction(
        state->chunk, VM_ENCODE_I(OP_NEW, rd, codegen_type_index(state, node->new_expr.type)));

    relocation_list_add(&state->unit->type_relocations,
                        (Relocation){.chunk = state->chunk, .offset = offset});

    return rd;
}

// ---- Field and pointer access ----

// Walks a field chain down to whatever the outermost struct lives in,
// accumulating the byte offsets on the way. Nested structs are inline, so
// 'outer.inner.x' is one base plus a single summed offset; a deref stops the
// walk, because from there on the base is an address computed at runtime.
//
// 'auto_deref' says whether a pointer at the bottom of the chain is reached
// through or addressed. Field access reaches through it — that is 'p.health'
// where p is a 'box Player' — but 'ref p' wants the address of p itself.
static FieldTarget codegen_resolve_field_target(CodegenState *state, ASTExpr *node, bool auto_deref) {
    if (node->kind == EXPR_FIELD) {
        ASTExpr *inner = node->field.target;

        // A pointer-typed field ends the chain rather than extending it: from
        // there the struct lives wherever that pointer says, so the pointer has
        // to be loaded and followed. Summing the offset instead would address
        // the pointer's own slot as if its inner were inline in it.
        //
        // Reachable only since a struct could hold a pointer, which is why
        // 'o.child.n' is the first expression to need this: it loads o.child,
        // then addresses n from there.
        if (inner->kind == EXPR_FIELD && type_is_indirect(inner->type)) {
            unsigned int base = codegen_field_expr(state, inner);

            return (FieldTarget){
                .base = base,
                .offset = node->field.field->offset,
                .indirect = true,
            };
        }

        // An element ends the chain for the same reason a pointer-typed field
        // does: its address is computed from the index, so a field of it is an
        // offset from that address rather than from any slot named here.
        //
        // A pointer-typed element is followed rather than offset into, exactly
        // as a pointer-typed field is: what 'xs[i].n' names then lives where
        // that pointer says, not in the block.
        if (inner->kind == EXPR_INDEX) {
            if (type_is_indirect(inner->type)) {
                return (FieldTarget){
                    .base = codegen_index_expr(state, inner),
                    .offset = node->field.field->offset,
                    .indirect = true,
                };
            }

            FieldTarget target = codegen_index_target(state, inner);
            target.offset += node->field.field->offset;
            return target;
        }

        // Everything else below a field access is a struct being reached into,
        // so a pointer there is always reached through.
        FieldTarget target = codegen_resolve_field_target(state, inner, true);
        target.offset += node->field.field->offset;
        return target;
    }

    if (node->kind == EXPR_DEREF) {
        unsigned int base = codegen_expr(state, node->unary.target);

        // A pointer to a pointer ends the chain the same way a pointer-typed
        // field does: what '*s' names is itself an address, so reaching past it
        // means loading it and following that. Summing an offset onto the outer
        // pointer would address the inner one's own slot as if the struct were
        // inline in it.
        TypeHandle reached = node->type;

        while (auto_deref && type_is_indirect(reached)) {
            base = codegen_load_indirect_struct(state, node, reached,
                                                (FieldTarget){
                                                    .base = base,
                                                    .offset = 0,
                                                    .indirect = true,
                                                },
                                                type_slot_count(reached));
            reached = type_pointee(reached);
        }

        return (FieldTarget){
            .base = base,
            .offset = 0,
            .indirect = true,
        };
    }

    unsigned int base = codegen_expr(state, node);

    // An allocation reached into but never bound: the field read below needs the
    // object, so the release waits for the statement rather than happening here.
    if (expr_yields_owned(node)) {
        owned_list_add(&state->temporaries,
                       (OwnedSlot){.slot = base, .depth = state->depth, .type = node->type});
        codegen_record_frame_ref(state, base, node->type);
    }

    bool indirect = auto_deref && type_is_indirect(node->type);

    // Each level above the last is loaded and followed: the innermost pointer is
    // what the field is addressed from, and the ones outside it only say where
    // that pointer lives. 'ref box Player' is the two-level case -- the borrow
    // names the slot, the slot names the object.
    if (indirect) {
        TypeHandle reached = node->type;

        while (type_is_indirect(type_pointee(reached))) {
            base = codegen_load_indirect_struct(state, node, type_pointee(reached),
                                                (FieldTarget){
                                                    .base = base,
                                                    .offset = 0,
                                                    .indirect = true,
                                                },
                                                type_slot_count(type_pointee(reached)));
            reached = type_pointee(reached);
        }
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
static unsigned int field_target_slot_count(FieldTarget target) {
    assert(!target.indirect && "an indirect struct field has no slot of its own");
    assert(target.offset % VM_SLOT_SIZE == 0 && "a struct field is always slot-aligned");

    return target.base + (unsigned int)(target.offset / VM_SLOT_SIZE);
}

// Copies a struct out of the address a pointer holds into fresh slots, so the
// result reads like any other struct-valued expression.
// 'type' is what lands in the destination, which is not always node->type: a
// method call derefs its receiver, and the node's own type is the return type.
static unsigned int codegen_load_indirect_struct(CodegenState *state, ASTExpr *node, TypeHandle type,
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
        address = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);
        chunk_add_instruction(state->chunk,
                              VM_ENCODE_R(OP_ADD_PTR, address, target.base, (unsigned int)target.offset));
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_LOAD_PTR_N, rd, address, slots));

    return rd;
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
        address = codegen_alloc_slots(state, VM_INDIRECT_SLOTS, VM_INDIRECT_SLOTS, node->span);
        chunk_add_instruction(state->chunk,
                              VM_ENCODE_R(OP_ADD_PTR, address, target.base, (unsigned int)target.offset));
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_STORE_PTR_N, address, src, slots));
}

static void codegen_store_field(CodegenState *state, ASTExpr *node, unsigned int src) {
    FieldTarget target = codegen_resolve_field_target(state, node, true);

    if (type_moves_as_slots(node->type)) {
        if (target.indirect) {
            codegen_store_indirect(state, node, target, src, type_slot_count(node->type));
            return;
        }

        codegen_copy_slots(state, field_target_slot_count(target), src, type_slot_count(node->type));
        return;
    }

    bool ok;
    OpCode op = field_opcode_for(node->field.field->type->size, false, target.indirect, &ok);

    if (!codegen_field_access_fits(state, node, ok, target.offset)) {
        return;
    }

    chunk_add_instruction(state->chunk, VM_ENCODE_R(op, target.base, src, (unsigned int)target.offset));
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

// 'ref x' materialises the address of whatever slots the target occupies. The
// target is addressable by construction: the resolver rejected anything else.
// The address of 'inner' written into 'rd'. Split from codegen_addr_of_expr so
// a method call can put a receiver's address straight into the argument slot it
// reserved, rather than into a slot of this function's choosing.
static void codegen_addr_of_into(CodegenState *state, ASTExpr *inner, unsigned int rd, Span span) {
    // 'ref p' where p is a pointer names p itself, so the chain must stop at it
    // rather than reach through.
    FieldTarget target = codegen_resolve_field_target(state, inner, false);

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

// ---- Binary operators ----

// Whether a right-hand operand can ride in the instruction's r2 field instead
// of being loaded into a register first.
//
// Only non-negative integer literals in range: r2 is eight unsigned bits, and
// widening it to hold a sign or a float would cost the field it shares with the
// register form. Everything else takes the register path unchanged, so this is
// an optimisation on the common shape rather than a restriction on the language.
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

// Whether this operator has a constant-operand opcode. Only the arithmetic
// four do, so this is what keeps codegen_rhs from choosing a form
// bin_op_opcode_for has no instruction for.
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

// The right operand of a binary op: a register, the value itself, or the pool
// index of the value. Reports which through 'kind', so the caller knows what
// instruction to emit.
//
// Generating it is what may allocate a register, so this runs before the result
// register is allocated -- the order the original codegen used, and the one the
// register numbering in the tests reflects.
static unsigned int codegen_rhs(CodegenState *state, BinOp op, ASTExpr *rhs, TypeHandle left_type,
                                RhsKind *kind) {
    unsigned int value = 0;

    if (left_type->kind == TYPE_FLOAT) {
        // A float literal is reached by index rather than by value: the operand
        // field is eight bits, and no float fits those.
        //
        // Only for an operator that has the form. The comparisons have no 'K'
        // opcode, so one reaching for it would have no instruction to name --
        // they take the register path below and load the literal first.
        if (bin_op_has_constant_form(op) && rhs->kind == EXPR_LITERAL && rhs->lit.kind == TYPE_FLOAT) {
            size_t index = constpool_add(state->chunk->const_pool, value_from_literal(rhs->lit));

            // Past what the field addresses, so this one is loaded as before.
            // Constants are pooled per function and deduplicated, so a chunk
            // reaching here has far more literals than anything hand-written.
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

// The instruction an operator and its right operand call for. A float literal
// takes the constant-pool form, and anything else the register or immediate
// form the k bit already distinguished.
static OpCode bin_op_opcode_for(BinOp op, TypeHandle left_type, RhsKind kind) {
    // A string is compared by its characters rather than its slots, so the
    // opcode is chosen by the operand type before the numeric families.
    if (left_type->kind == TYPE_STRING) {
        return op == BIN_OP_EQUAL ? OP_CMP_EQS : OP_CMP_NES;
    }

    if (kind != RHS_CONSTANT) {
        return left_type->kind == TYPE_FLOAT ? bin_op_to_float_op(op) : bin_op_to_int_op(op);
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

    // Unreachable: codegen_rhs chooses RHS_CONSTANT only for the operators
    // above, so the two switches state the same set and this is what catches
    // them drifting apart rather than a case a program can reach.
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

// Emits one arithmetic or comparison instruction. Shared by both binary-op
// paths so the two cannot disagree about how the operand is encoded.
static void codegen_emit_bin_op(CodegenState *state, ASTExpr *node, unsigned int dest, unsigned int lhs,
                                unsigned int rhs, RhsKind kind) {
    OpCode op_code = bin_op_opcode_for(node->bin_op.op, node->bin_op.left->type, kind);

    chunk_add_instruction(state->chunk, VM_ENCODE_RK(op_code, dest, lhs, rhs, kind == RHS_IMMEDIATE ? 1 : 0));
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

    // A concatenation lands in a string's worth of slots rather than one, and
    // its result owns, so it takes neither the immediate encoding nor the
    // single-register destination the arithmetic path allocates.
    if (node->bin_op.op == BIN_OP_CONCAT) {
        unsigned int left = codegen_expr(state, node->bin_op.left);
        unsigned int right = codegen_expr(state, node->bin_op.right);
        unsigned int result = codegen_alloc_slots(state, VM_STRING_SLOTS, VM_INDIRECT_SLOTS, node->span);

        chunk_add_instruction(state->chunk, VM_ENCODE_R(OP_CONCAT, result, left, right));

        // Whoever consumes this may bind it -- a 'let' takes ownership of the
        // slot -- but an operand position does not, and nothing else would free
        // it. Recorded as a temporary, which the statement's end releases and
        // binding disowns.
        owned_list_add(&state->temporaries,
                       (OwnedSlot){.slot = result, .depth = state->depth, .type = node->type});
        codegen_record_frame_ref(state, result, node->type);

        return result;
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

// ---- Forward jumps and loops ----

static CodegenLabel codegen_create_label(CodegenState *state) {
    return (CodegenLabel){.position = chunk_add_instruction(state->chunk, 0)};
}

static void codegen_patch_jump(CodegenState *state, CodegenLabel label, OpCode op, unsigned int reg) {
    Instruction patch = VM_ENCODE_I(op, reg, state->chunk->instructions.size - label.position - 1);
    chunk_patch_instruction(state->chunk, label.position, patch);
}

// Jumps back to an instruction index already emitted. The offset is negative,
// which an ordinary OP_JMP carries: it is measured from the instruction after
// this one, the point the interpreter has reached by the time it jumps.
static void codegen_emit_loop(CodegenState *state, size_t target) {
    size_t position = chunk_add_instruction(state->chunk, 0);
    ptrdiff_t offset = (ptrdiff_t)target - (ptrdiff_t)(position + 1);

    chunk_patch_instruction(state->chunk, position, VM_ENCODE_I(OP_JMP, 0, offset));
}

// ---- Reference ownership ----

// Whether an expression hands its caller a reference to own, or merely lends
// one it keeps. 'new' and a call returning 'box T' produce a fresh reference the
// receiver is responsible for; reading a variable or a field does not, since
// the variable or the object still holds it.
//
// Derived from the expression rather than recorded on it: the two producing
// forms are exactly these, and a flag on every node would have to be kept in
// step with them for no extra information.
static bool expr_yields_owned(const ASTExpr *expr) {
    // Copyability is the same question inverted: a value carries ownership
    // exactly when it cannot be duplicated by copying its bytes. A struct
    // qualifies through its fields -- 'move h' on a struct holding an owning
    // pointer hands that pointer over, though the struct is not one.
    if (!expr || type_is_copyable(expr->type)) {
        return false;
    }

    switch (expr->kind) {
    // A concatenation allocates the characters it yields; every other binary op
    // answers with a value that owns nothing.
    case EXPR_BIN_OP:
        return expr->bin_op.op == BIN_OP_CONCAT;

    case EXPR_NEW:
    case EXPR_CALL:

    // A move hands the source's reference over, so the destination owns it
    // exactly as it would a fresh one -- and the source has been disowned, so
    // no second slot will free it.
    case EXPR_MOVE:
        return true;
    default:
        return false;
    }
}

// Records that this slot may hold a reference when the frame unwinds. A failure
// jumps past every free emitted below, so the frame has to know what it may
// still be holding.
//
// Recorded once per slot: a slot reused by a later block is the same slot, and
// the runtime clears one when it frees it, so a second entry would only make the
// unwinder visit an empty slot twice.
static void codegen_record_frame_ref(CodegenState *state, unsigned int slot, TypeHandle type) {
    for (size_t i = 0; i < state->frame_refs.size; i++) {
        if (state->frame_refs.data[i].slot == slot) {
            return;
        }
    }

    frame_ref_list_add(&state->frame_refs, (FrameRef){.slot = slot, .type = type});
}

static void codegen_own_slot_at(CodegenState *state, unsigned int slot, TypeHandle type, unsigned int depth) {
    // A 'ref T' slot borrows, so it never owns and is never freed. Callers
    // check this too, but a stray own here would be a use-after-free rather
    // than a leak, so it is refused at the one place ownership is recorded.
    assert(!(type && type->kind == TYPE_REF) && "a 'ref T' slot never owns what it names");

    owned_list_add(&state->owned, (OwnedSlot){.slot = slot, .depth = depth, .type = type});

    // Also recorded on the frame, for the unwinder.
    codegen_record_frame_ref(state, slot, type);
}

// As codegen_own_slot_at, at the depth of the block being generated. For a slot
// the current block itself owns: a temporary, or a parameter placed at entry.
static void codegen_own_slot(CodegenState *state, unsigned int slot, TypeHandle type) {
    codegen_own_slot_at(state, slot, type, state->depth);
}

// Forgets that a slot owns, without emitting a release. For 'move': the
// reference is now the destination's, so the source must not free it when its
// block closes. The frame record stays, since the unwinder reads the slot
// itself and the destination is what holds the reference now.
static void codegen_disown_slot(CodegenState *state, unsigned int slot) {
    for (size_t i = 0; i < state->owned.size; i++) {
        if (state->owned.data[i].slot == slot) {
            // Order matters: releases sweep innermost-first, so the
            // remaining entries must keep the order they were added in.
            for (size_t j = i + 1; j < state->owned.size; j++) {
                state->owned.data[j - 1] = state->owned.data[j];
            }

            state->owned.size--;
            return;
        }
    }
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

        codegen_emit_release(state, owned.slot, owned.type);
    }
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

        codegen_emit_release(state, owned.slot, owned.type);
    }
}

// ---- Frame slots and registers ----

// The frame slot a symbol was given. Every read is of a slot this same
// function body assigned, so a miss is a codegen bug rather than a user error.
static unsigned int codegen_slot_of(CodegenState *state, Symbol *symbol) {
    SlotBinding *slot = slot_map_lookup(state->slots, symbol);

    assert(slot && "symbol was never assigned a frame slot");

    return slot->slot;
}

// The depth of the block that declared this name, for an ownership record that
// must outlive the block doing the assigning.
static unsigned int codegen_decl_depth_of(CodegenState *state, Symbol *symbol) {
    SlotBinding *slot = slot_map_lookup(state->slots, symbol);

    assert(slot && "symbol was never assigned a frame slot");

    return slot->depth;
}

static void codegen_set_slot(CodegenState *state, Symbol *symbol, unsigned int slot) {
    slot_map_insert(state->slots, symbol, (SlotBinding){.slot = slot, .depth = state->depth});
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

// ---- Type layout in frame slots ----

// A value occupies ceil(size / 4) consecutive slots, which is 1 for every
// scalar.
static unsigned int type_slot_count(TypeHandle type) {
    if (!type) {
        return 1;
    }

    return (unsigned int)((type->size + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);
}

// Slot alignment a value of this type needs. A scalar wants one; an 8-byte
// pointer wants two, so that it lands on its natural alignment.
static unsigned int type_align_slots(TypeHandle type) {
    if (!type || type->alignment <= VM_SLOT_SIZE) {
        return 1;
    }

    return (unsigned int)(type->alignment / VM_SLOT_SIZE);
}

static bool type_is_struct(TypeHandle type) { return type && type->kind == TYPE_STRUCT; }

// A struct is written through its fields and an array through its elements: the
// slot goes on holding the same value while what it owns changes underneath, so
// no assignment would ever record it. A pointer is the other case -- assigning
// one replaces the value, and that is where it takes ownership.
//
// Derived from the same two questions the owning walk asks rather than from a
// list of kinds, so a shape that starts owning through members cannot be added
// to one and forgotten in the other.
static bool type_owns_through_members(TypeHandle type) {
    return type_is_owned(type) && !type_is_indirect(type);
}

// Whether a field is moved as a run of slots rather than through a width-tagged
// field opcode. Anything laid out from fields is, because those slots sit
// inline — a struct and a string alike — and so is a pointer, which is 8 bytes
// and has no 8-wide opcode. Before heap objects existed no struct could hold a
// pointer, so the two cases only meet now.
static bool type_moves_as_slots(TypeHandle type) {
    return type_is_indirect(type) || (type && type_field_count(type) > 0);
}

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
