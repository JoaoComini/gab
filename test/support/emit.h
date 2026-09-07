#ifndef GAB_TEST_EMIT_H
#define GAB_TEST_EMIT_H

#include "mir/mir_drop.h"
#include "mir/mir_fold.h"
#include "string/string_ref.h"
#include "support/run.h"
#include "vm/codegen_mir.h"

#include <assert.h>

typedef struct {
    TestContext ctx;
    Scope *scope;
    ASTUnit *unit;
    MIRModule *mir_unit;
    ResolvedUnit *resolved;

    MIRFunction *ir;
    Chunk *chunk;

    unsigned int max_registers;

    FrameRefList refs;
} TestEmission;

/* Lowers and folds one function a source declares, named where a test means other than the first. */
static inline TestEmission test_lower_ir_named(const char *source, const char *name) {
    TestEmission emission = {0};

    test_context_init(&emission.ctx);

    emission.scope = scope_create(emission.ctx.arena, &emission.ctx.strings, NULL);
    emission.unit = ast_unit_create(emission.ctx.arena);
    bool resolved = test_resolve_ir(&emission.ctx, emission.scope, &emission.unit, &emission.mir_unit,
                                    &emission.resolved, source);

    assert(resolved);

    for (size_t i = 0; i < emission.unit->statements.size; i++) {
        ASTStmt *stmt = emission.unit->statements.data[i];

        if (!stmt || stmt->kind != STMT_FUNC_DECL || !stmt->func_decl.body) {
            continue;
        }

        if (name && !string_ref_equals_cstr(stmt->func_decl.name, name)) {
            continue;
        }

        emission.ir = mir_module_lookup(emission.mir_unit, stmt->func_decl.function);

        break;
    }

    assert(emission.ir);

    mir_fold(emission.ctx.arena, emission.ir);
    mir_drop_elaborate(emission.ctx.arena, emission.scope->type_registry, emission.ir);

    return emission;
}

static inline TestEmission test_lower_ir(const char *source) { return test_lower_ir_named(source, NULL); }

/* Numbers whatever a body names, so an emission test needs no unit to have assigned indices. */
static inline bool test_any_callee_index(void *context, Function *callee, bool native, unsigned int *index,
                                         bool *relocates) {
    *relocates = false;

    (void)context;
    (void)callee;
    (void)native;

    *index = 0;

    return true;
}

static inline bool test_any_heap_shape(void *context, const Type *type, unsigned int *index) {
    (void)context;
    (void)type;

    *index = 0;

    return true;
}

/* Interns by identity, since the pool already gives equal text one String. */
static inline bool test_any_string(void *context, String *text, unsigned int *index) {
    (void)context;

    static String *seen[64];
    static unsigned int count;

    for (unsigned int i = 0; i < count; i++) {
        if (seen[i] == text) {
            *index = i;

            return true;
        }
    }

    seen[count] = text;
    *index = count++;

    return true;
}

static inline void test_no_relocation(void *context, Chunk *chunk, size_t offset) {
    (void)context;
    (void)chunk;
    (void)offset;
}

#define TEST_ANY_NUMBERING                                                                                   \
    (MIRUnitNumbering) {                                                                                     \
        .context = NULL, .callee = test_any_callee_index, .heap_shape = test_any_heap_shape,                 \
        .relocate_type = test_no_relocation, .string = test_any_string,                                      \
        .relocate_string = test_no_relocation                                                                \
    }

static inline TestEmission test_emit_ir_named(const char *source, const char *name) {
    TestEmission emission = test_lower_ir_named(source, name);

    MIRUnitNumbering numbering = TEST_ANY_NUMBERING;

    MIREmission emitted =
        codegen_mir_emit(emission.ctx.arena, emission.ir, &numbering, &emission.ctx.diagnostics);

    assert(!emitted.failed);

    emission.chunk = emitted.chunk;
    emission.max_registers = emitted.max_registers;
    emission.refs = emitted.refs;

    return emission;
}

static inline void test_emission_free(TestEmission *emission);

static inline TestEmission test_emit_ir(const char *source) { return test_emit_ir_named(source, NULL); }

/* Numbers each function a unit declares, so a call names the prototype the run installed for it. */
typedef struct {
    Function *functions[64];
    size_t count;

    /* Where this unit's prototypes land, since the prelude installed its own ahead of them. */
    size_t base;

    /* The shapes a boxed type needs at run time, which the program reads a box's index out of. */
    TypeRegistry *registry;
    HeapShapeList *shapes;
    const Type *types[64];
    size_t type_count;
} TestCallees;

static inline bool test_unit_heap_shape(void *context, const Type *type, unsigned int *index) {
    TestCallees *callees = (TestCallees *)context;

    for (size_t i = 0; i < callees->type_count; i++) {
        if (callees->types[i] == type) {
            *index = (unsigned int)i;

            return true;
        }
    }

    callees->types[callees->type_count++] = type;

    heap_shape_list_add(callees->shapes, (HeapShape){
                                             .size = type_registry_size_of(callees->registry, type),
                                             .drop = type_registry_drop_of(callees->registry, type),
                                             .release_width = type_registry_size_of(callees->registry, type),
                                         });

    *index = (unsigned int)(callees->type_count - 1);

    return true;
}

static inline bool test_callee_index(void *context, Function *callee, bool native, unsigned int *index,
                                     bool *relocates) {
    *relocates = false;

    TestCallees *callees = (TestCallees *)context;

    (void)native;

    for (size_t i = 0; i < callees->count; i++) {
        if (callees->functions[i] == callee) {
            *index = (unsigned int)(callees->base + i);

            return true;
        }
    }

    return false;
}

/* Emits every function a unit declares and runs the last, so a call reaches a body that exists. */
/* Resolves against a VM, so the prelude's interfaces and its slice methods are in scope. */
static inline int32_t test_run_emitted_unit(const char *source, VmRunStatus *out_status) {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    ASTUnit *unit;

    assert(
        parse_unit(test_in_a_module(source), vm->env.compile_arena, &vm->env.strings, &unit, &diagnostics));

    Scope staging;
    scope_init_staging(&staging, vm->env.arena, &vm->env.strings,
                       environment_module_scope(&vm->env, string_from_cstr(&vm->env.strings, "test")));

    Scope *scope = &staging;

    ResolvedUnit *resolved;

    assert(resolve_unit(vm->env.compile_arena, unit, scope, vm->env.module_scopes, false, &resolved,
                        &diagnostics));

    MIRModule *mir_unit;
    mir_build(vm->env.compile_arena, resolved, &mir_unit, &diagnostics);

    TestCallees callees = {.registry = scope->type_registry,
                           .shapes = &vm->program.heap_shapes,
                           .base = vm->program.prototypes.size};

    for (size_t i = 0; i < unit->statements.size; i++) {
        ASTStmt *stmt = unit->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL && stmt->func_decl.body) {
            callees.functions[callees.count++] = stmt->func_decl.function;
        }
    }

    MIRUnitNumbering numbering = {.context = &callees,
                                  .callee = test_callee_index,
                                  .heap_shape = test_unit_heap_shape,
                                  .relocate_type = test_no_relocation,
                                  .string = test_any_string,
                                  .relocate_string = test_no_relocation};

    FuncPrototype **protos = (FuncPrototype **)calloc(callees.count, sizeof(FuncPrototype *));

    for (size_t i = 0; i < callees.count; i++) {
        MIRFunction *ir = mir_module_lookup(mir_unit, callees.functions[i]);

        mir_fold(vm->env.compile_arena, ir);
        mir_drop_elaborate(vm->env.compile_arena, scope->type_registry, ir);

        MIREmission emitted = codegen_mir_emit(vm->env.compile_arena, ir, &numbering, &diagnostics);

        assert(!emitted.failed);

        protos[i] = (FuncPrototype *)calloc(1, sizeof(FuncPrototype));

        *protos[i] = (FuncPrototype){
            .chunk = emitted.chunk,
            .arity = (int)ir->param_count,
            .max_registers = (int)emitted.max_registers,
            .refs = emitted.refs,
        };

        func_proto_list_add(&vm->program.prototypes, protos[i]);
    }

    VmRunStatus status = interp_run_top_level(vm, protos[callees.count - 1]);

    if (out_status) {
        *out_status = status;
    } else {
        assert(status == VM_RUN_OK);
    }

    int32_t result;
    memcpy(&result, vm_slot_at(vm, 0), sizeof(result));

    diagnostics_free(&diagnostics);

    /* The program frees what each prototype holds, leaving the structs themselves to their allocator. */
    vm_free(vm);

    for (size_t i = 0; i < callees.count; i++) {
        free(protos[i]);
    }

    free(protos);

    return result;
}

static inline int32_t test_run_emitted_unit_int(const char *source) {
    return test_run_emitted_unit(source, NULL);
}

static inline VmRunStatus test_run_emitted_unit_status(const char *source) {
    VmRunStatus status;

    test_run_emitted_unit(source, &status);

    return status;
}

/* Runs an emitted body as a frame of its own, so a test can assert what the chunk computes. */
static inline int32_t test_run_emitted_int(const char *source) {
    TestEmission emission = test_emit_ir(source);

    FuncPrototype proto = {
        .chunk = emission.chunk,
        .arity = 0,
        .max_registers = (int)emission.max_registers,
        .refs = emission.refs,
    };

    VM *vm = vm_create();

    VmRunStatus status = interp_run_top_level(vm, &proto);

    assert(status == VM_RUN_OK);

    int32_t result;
    memcpy(&result, vm_slot_at(vm, 0), sizeof(result));

    vm_free(vm);
    frame_ref_list_free(&proto.refs);
    test_emission_free(&emission);

    return result;
}

/* Runs an emitted body for its status, so a test can state which programs trap. */
static inline VmRunStatus test_run_emitted_status(const char *source) {
    TestEmission emission = test_emit_ir(source);

    FuncPrototype proto = {
        .chunk = emission.chunk,
        .arity = 0,
        .max_registers = (int)emission.max_registers,
        .refs = emission.refs,
    };

    VM *vm = vm_create();

    VmRunStatus status = interp_run_top_level(vm, &proto);

    vm_free(vm);
    frame_ref_list_free(&proto.refs);
    test_emission_free(&emission);

    return status;
}

static inline void test_emission_free(TestEmission *emission) {
    if (emission->chunk) {
        chunk_free(emission->chunk);
        frame_ref_list_free(&emission->refs);
    }

    test_context_free(&emission->ctx);
}

#endif
