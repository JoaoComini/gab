#ifndef GAB_TEST_EMIT_H
#define GAB_TEST_EMIT_H

#include "mir/mir_drop.h"
#include "mir/mir_fold.h"
#include "string/string_ref.h"
#include "support/run.h"

#include <assert.h>

typedef struct {
    TestContext ctx;
    Scope *scope;
    ASTModule *unit;
    MIRModule *mir_unit;
    ResolvedModule *resolved;

    ModuleScopeMap *module_scopes;

    MIRFunction *ir;
} TestEmission;

/* Lowers and folds one function a source declares, named where a test means other than the first. */
static inline TestEmission test_lower_ir_named(const char *source, const char *name) {
    TestEmission emission = {0};

    test_context_init(&emission.ctx);

    emission.scope = scope_create(emission.ctx.arena, &emission.ctx.strings, NULL);
    emission.unit = ast_module_create(emission.ctx.arena);
    bool resolved = test_resolve_ir(&emission.ctx, emission.scope, &emission.unit, &emission.mir_unit,
                                    &emission.resolved, source);

    assert(resolved);

    for (size_t i = 0; i < ast_module_statements(emission.unit)[0].size; i++) {
        ASTStmt *stmt = ast_module_statements(emission.unit)[0].data[i];

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
    mir_drop_elaborate(emission.ctx.arena, emission.scope->type_registry, emission.scope->functions,
                       emission.ir);

    return emission;
}

static inline TestEmission test_lower_ir(const char *source) { return test_lower_ir_named(source, NULL); }

/* Every body a source declares, lowered together, so a unit emits what its calls reach. */
static inline TestEmission test_lower_unit(const char *source, MIRFunction **out, size_t capacity,
                                           size_t *count) {
    TestEmission emission = {0};

    test_context_init(&emission.ctx);

    emission.scope = scope_create(emission.ctx.arena, &emission.ctx.strings, NULL);
    emission.unit = ast_module_create(emission.ctx.arena);

    bool resolved = test_resolve_ir_with(&emission.ctx, emission.scope, &emission.unit, &emission.mir_unit,
                                         &emission.resolved, source, true);

    assert(resolved);

    *count = 0;

    /* Every lowered body, rather than every top-level statement, so a method an 'impl' block declares
     * is emitted alongside the functions that call it. */
    for (size_t i = 0; i < emission.mir_unit->entries.size && *count < capacity; i++) {
        MIRFunction *ir = emission.mir_unit->entries.data[i].ir;

        if (!ir || mir_function_is_template(ir)) {
            continue;
        }

        mir_fold(emission.ctx.arena, ir);
        mir_drop_elaborate(emission.ctx.arena, emission.scope->type_registry, emission.scope->functions, ir);

        out[(*count)++] = ir;
    }

    return emission;
}

static inline void test_emission_free(TestEmission *emission) { test_context_free(&emission->ctx); }

#endif
