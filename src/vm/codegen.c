#include "codegen.h"

#include "ast/ast.h"
#include "ast/expr.h"
#include "ast/stmt.h"
#include "mir/mir_drop.h"
#include "mir/mir_fold.h"
#include "scope.h"
#include "type/type.h"
#include "type/type_layout.h"
#include "vm/chunk.h"
#include "vm/codegen_mir.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include <assert.h>
#include <stdlib.h>

#define PROTO_MAP_INITIAL_CAPACITY 16

#define proto_map_hash(key) (size_t)key
#define proto_map_key_equals(key, other) key == other

GAB_HASH_MAP(ProtoMap, proto_map, Function *, size_t)

typedef struct {
    Chunk *chunk;
    unsigned int next_reg;

    unsigned int max_reg;

    ObjectFile *unit;
    Arena *arena;

    TypeRegistry *registry;

    StringPool *strings;

    const MIRModule *mir_unit;

    ProtoMap *local_protos;

    FrameRefList frame_refs;

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

static size_t codegen_reserve_function(CodegenState *state, Function *function);
static bool codegen_emits_instance(const Function *function);
static void codegen_reserve_proto(CodegenState *state, ASTFuncDecl *ast);
static void codegen_func_decl_stmt(CodegenState *state, ASTStmt *stmt);
static void codegen_emit_body(CodegenState *state, Function *function, const ASTFieldList *params,
                              size_t func_index, Span span);
static bool codegen_emit_from_ir(CodegenState *state, Function *function, MIREmission *out);

static unsigned int codegen_load_indirect_struct(CodegenState *state, ASTExpr *node, const Type *type,
                                                 FieldTarget target, unsigned int slots);
static void codegen_store_indirect(CodegenState *state, ASTExpr *node, FieldTarget target, unsigned int src,
                                   unsigned int slots);

static unsigned int codegen_rhs(CodegenState *state, BinOp op, ASTExpr *rhs, const Type *left_type,
                                RhsKind *kind);
static void codegen_emit_bin_op(CodegenState *state, ASTExpr *node, unsigned int dest, unsigned int lhs,
                                unsigned int rhs, RhsKind kind);

static unsigned int codegen_type_index(CodegenState *state, const Type *type);

static unsigned int codegen_alloc_slots(CodegenState *state, unsigned int count, unsigned int align_slots,
                                        Span span);

static size_t slot_release_width(TypeRegistry *registry, const Type *type);

bool codegen_generate(ASTUnit *ast, Arena *arena, StringPool *strings, TypeRegistry *registry,
                      const MIRModule *mir_unit, ObjectFile **out, Diagnostics *diagnostics) {
    ObjectFile *unit = calloc(1, sizeof(ObjectFile));

    if (!unit) {
        return false;
    }

    unit->prototypes = func_proto_list_create(arena_allocator(arena));
    unit->types = type_list_create(arena_allocator(arena));
    unit->type_shapes = heap_shape_list_create(arena_allocator(arena));
    unit->strings = string_list_create(arena_allocator(arena));
    unit->proto_relocations = relocation_list_create(arena_allocator(arena));
    unit->type_relocations = relocation_list_create(arena_allocator(arena));
    unit->string_relocations = relocation_list_create(arena_allocator(arena));
    unit->bindings = proto_binding_list_create(arena_allocator(arena));
    unit->arena = arena;

    CodegenState state = {
        .chunk = chunk_create(),
        .next_reg = 0,
        .max_reg = 0,
        .unit = unit,
        .arena = arena,
        .registry = registry,
        .strings = strings,
        .mir_unit = mir_unit,
        .local_protos = proto_map_create(PROTO_MAP_INITIAL_CAPACITY),
        .frame_refs = frame_ref_list_create(arena_allocator(arena)),
        .diagnostics = diagnostics,
        .failed = false,
    };

    for (size_t i = 0; i < ast->statements.size; i++) {
        ASTStmt *stmt = ast->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL) {
            codegen_reserve_proto(&state, &stmt->func_decl);
        }

        if (stmt && stmt->kind == STMT_IMPL) {
            for (size_t m = 0; m < stmt->impl.members.size; m++) {
                codegen_reserve_proto(&state, &stmt->impl.members.data[m]->func_decl);
            }
        }
    }

    /* Reserved before any body is emitted, since a declared body may call an instance. */
    for (size_t i = 0; mir_unit && i < mir_unit->entries.size; i++) {
        Function *function = mir_unit->entries.data[i].function;

        if (codegen_emits_instance(function) && !proto_map_lookup(state.local_protos, function)) {
            codegen_reserve_function(&state, function);
        }
    }

    /* A declaration binds a body the unit must hold; what a script runs is lowered with the rest. */
    for (size_t i = 0; i < ast->statements.size; i++) {
        ASTStmt *stmt = ast->statements.data[i];

        if (!stmt) {
            continue;
        }

        if (stmt->kind == STMT_FUNC_DECL) {
            codegen_func_decl_stmt(&state, stmt);
        }

        if (stmt->kind == STMT_IMPL) {
            for (size_t m = 0; m < stmt->impl.members.size; m++) {
                codegen_func_decl_stmt(&state, stmt->impl.members.data[m]);
            }
        }
    }

    for (size_t i = 0; mir_unit && i < mir_unit->entries.size; i++) {
        Function *function = mir_unit->entries.data[i].function;

        const size_t *local =
            codegen_emits_instance(function) ? proto_map_lookup(state.local_protos, function) : NULL;

        if (local) {
            codegen_emit_body(&state, function, NULL, *local, (Span){0});
        }
    }

    MIREmission top = {0};

    if (codegen_emit_from_ir(&state, NULL, &top)) {
        chunk_free(state.chunk);
        frame_ref_list_free(&state.frame_refs);

        state.chunk = top.chunk;
        state.max_reg = top.max_registers;
        state.frame_refs = top.refs;

        unit->top_level.result_slot = (int)top.first_local_slot;
    }

    OpCode last = state.chunk->instructions.size > 0
                      ? VM_DECODE_OPCODE(instruction_list_back(&state.chunk->instructions))
                      : OP_LOAD_CONST;

    if (state.chunk->instructions.size == 0 || (last != OP_RETURN && last != OP_RETURN_N)) {
        chunk_add_instruction(state.chunk, VM_ENCODE_R(OP_RETURN, 0, 0, 0));
    }

    proto_map_destroy(state.local_protos);

    if (state.failed) {
        chunk_free(state.chunk);
        frame_ref_list_free(&state.frame_refs);
        object_file_free(unit);
        return false;
    }

    unit->top_level.chunk = state.chunk;
    unit->top_level.max_registers = (int)state.max_reg;
    unit->top_level.refs = state.frame_refs;

    *out = unit;

    return true;
}

typedef enum {
    OWNING_SLOT_NULL,

    OWNING_SLOT_OWN,

    OWNING_SLOT_DISOWN,
} OwningSlotAction;

static size_t codegen_reserve_function(CodegenState *state, Function *function) {
    size_t local;

    assert(function->decl->body_kind != BODY_INTRINSIC && "an intrinsic is lowered, never called");

    FuncPrototype *proto = arena_alloc(state->arena, sizeof(FuncPrototype));
    *proto = (FuncPrototype){0};

    func_proto_list_add(&state->unit->prototypes, proto);

    local = state->unit->prototypes.size - 1;

    proto_map_insert(state->local_protos, function, local);
    proto_binding_list_add(&state->unit->bindings,
                           (ProtoBinding){.function = function, .local_index = local});

    return local;
}

/* An instance has no declaration of its own, so what was lowered for it is what codegen walks. */
static bool codegen_emits_instance(const Function *function) {
    return function && function->type_arg_count > 0 && function->decl->body_kind != BODY_INTRINSIC;
}

/* A generic declaration is a template rather than a body, so what it emits is its instances. */
static bool codegen_is_template(const ASTFuncDecl *ast) {
    return (ast->owner && ast->owner->kind == TYPE_EXPR_APPLY) || ast->type_param_count > 0;
}

static void codegen_reserve_proto(CodegenState *state, ASTFuncDecl *ast) {
    /* An intrinsic is lowered where it is called, so it reserves no prototype and binds no body. */
    if (!ast->function || ast->function->decl->body_kind == BODY_INTRINSIC || codegen_is_template(ast) ||
        proto_map_lookup(state->local_protos, ast->function)) {
        return;
    }

    codegen_reserve_function(state, ast->function);
}

static void codegen_func_decl_stmt(CodegenState *state, ASTStmt *stmt) {
    ASTFuncDecl *ast = &stmt->func_decl;

    if (ast->function && ast->function->decl->body_kind == BODY_INTRINSIC) {
        return;
    }

    codegen_reserve_proto(state, ast);

    if (!ast->function) {
        return;
    }

    const size_t *local = proto_map_lookup(state->local_protos, ast->function);

    if (!local) {
        return;
    }

    size_t func_index = *local;

    /* A body the linker supplies has no chunk here, so nothing is emitted into its slot. */
    if (ast->function->decl->body_kind == BODY_HOST) {
        return;
    }

    codegen_emit_body(state, ast->function, &ast->params, func_index, stmt->span);
}

/* The unit numbers what a lowered body names, which only codegen has assigned an index to. */
static bool codegen_mir_callee(void *context, Function *callee, unsigned int *index, bool *relocates) {
    CodegenState *state = (CodegenState *)context;

    const size_t *local = proto_map_lookup(state->local_protos, callee);

    size_t at = local ? *local : callee->func_index;

    if (at == FUNCTION_NO_BODY) {
        return false;
    }

    if (!local && at > VM_MAX_PROTOTYPES) {
        return false;
    }

    *index = (unsigned int)at;
    *relocates = local != NULL;

    return true;
}

static void codegen_mir_relocate_callee(void *context, Chunk *chunk, size_t offset) {
    CodegenState *state = (CodegenState *)context;

    relocation_list_add(&state->unit->proto_relocations, (Relocation){.chunk = chunk, .offset = offset});
}

static bool codegen_mir_heap_shape(void *context, const Type *type, unsigned int *index) {
    CodegenState *state = (CodegenState *)context;

    *index = codegen_type_index(state, type);

    return true;
}

static void codegen_mir_relocate_type(void *context, Chunk *chunk, size_t offset) {
    CodegenState *state = (CodegenState *)context;

    relocation_list_add(&state->unit->type_relocations, (Relocation){.chunk = chunk, .offset = offset});
}

static bool codegen_mir_string(void *context, String *text, unsigned int *index) {
    CodegenState *state = (CodegenState *)context;

    for (size_t i = 0; i < state->unit->strings.size; i++) {
        if (state->unit->strings.data[i] == text) {
            *index = (unsigned int)i;

            return true;
        }
    }

    string_list_add(&state->unit->strings, text);

    *index = (unsigned int)(state->unit->strings.size - 1);

    return true;
}

static void codegen_mir_relocate_string(void *context, Chunk *chunk, size_t offset) {
    CodegenState *state = (CodegenState *)context;

    relocation_list_add(&state->unit->string_relocations, (Relocation){.chunk = chunk, .offset = offset});
}

/* Emits what was lowered for one body, leaving where the result lands to the caller. */
static bool codegen_emit_from_ir(CodegenState *state, Function *function, MIREmission *out) {
    MIRFunction *ir = state->mir_unit ? mir_module_lookup(state->mir_unit, function) : NULL;

    if (!ir) {
        return false;
    }

    mir_fold(state->arena, ir);
    mir_drop_elaborate(state->arena, state->registry, ir);

    if (!codegen_mir_supports(ir)) {
        return false;
    }

    MIRUnitNumbering numbering = {
        .context = state,
        .callee = codegen_mir_callee,
        .relocate_callee = codegen_mir_relocate_callee,
        .heap_shape = codegen_mir_heap_shape,
        .relocate_type = codegen_mir_relocate_type,
        .string = codegen_mir_string,
        .relocate_string = codegen_mir_relocate_string,
    };

    Chunk *saved = state->chunk;

    *out = codegen_mir_emit(state->arena, ir, &numbering, state->diagnostics);

    state->chunk = saved;

    return !out->failed;
}

/* A body is emitted from what was lowered for it, into the prototype its callers already reserved. */
static bool codegen_emit_body_from_ir(CodegenState *state, Function *function, size_t func_index,
                                      size_t arity) {
    MIREmission emitted;

    if (!codegen_emit_from_ir(state, function, &emitted)) {
        return false;
    }

    *state->unit->prototypes.data[func_index] = (FuncPrototype){
        .chunk = emitted.chunk,
        .arity = (int)arity,
        .max_registers = (int)emitted.max_registers,
        .refs = emitted.refs,
    };

    return true;
}

static void codegen_emit_body(CodegenState *state, Function *function, const ASTFieldList *params,
                              size_t func_index, Span span) {
    /* An instance has no declaration of its own to count, so its arity is the one its signature states. */
    size_t arity = params ? params->size : function->param_count;

    if (codegen_emit_body_from_ir(state, function, func_index, arity)) {
        return;
    }

    /* Every body the language accepts is emitted from its IR; a body reaching here is one the
     * lowering or the emitter does not yet cover, which is a bug rather than a shape to walk. */
    diag_error(state->diagnostics, GAB_ERR_CODEGEN, span, "this body has no emission from its IR");

    state->failed = true;
}

/* A slice is where the elements start and how many there are, in the two slots a '&slice<T>' occupies. */
/* An array states its length in its type; a slice carries it in the slot past the address it holds. */
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

/* A value copied into the heap leaves its registers non-owning, fields included. */
static size_t slot_release_width(TypeRegistry *registry, const Type *type) {
    return type_registry_size_of(registry, type);
}
