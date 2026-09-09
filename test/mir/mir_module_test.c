#include "mir/mir_module.h"
#include "support/run.h"

#include <assert.h>
#include <stdio.h>

static void resolving_a_unit_lowers_every_function(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTModule *unit = ast_module_create(ctx.arena);
    MIRModule *mir_unit = NULL;

    assert(test_resolve_ir(&ctx, scope, &unit, &mir_unit, NULL,
                           "func one(): i32 { return 1; }\n"
                           "func two(): i32 { return 2; }\n"));

    for (size_t i = 0; i < ast_module_statements(unit)[0].size; i++) {
        ASTStmt *stmt = ast_module_statements(unit)[0].data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL && stmt->func_decl.body) {
            assert(mir_module_lookup(mir_unit, stmt->func_decl.function));
        }
    }

    test_context_free(&ctx);
}

static void a_function_the_unit_never_resolved_is_absent(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTModule *unit = ast_module_create(ctx.arena);
    MIRModule *mir_unit = NULL;

    assert(test_resolve_ir(&ctx, scope, &unit, &mir_unit, NULL, "func one(): i32 { return 1; }\n"));

    Function absent = {0};

    assert(!mir_module_lookup(mir_unit, &absent));

    test_context_free(&ctx);
}

static void a_lowered_body_carries_the_function_it_came_from(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTModule *unit = ast_module_create(ctx.arena);
    MIRModule *mir_unit = NULL;

    assert(test_resolve_ir(&ctx, scope, &unit, &mir_unit, NULL, "func one(a: i32): i32 { return a; }\n"));

    for (size_t i = 0; i < ast_module_statements(unit)[0].size; i++) {
        ASTStmt *stmt = ast_module_statements(unit)[0].data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL && stmt->func_decl.body) {
            MIRFunction *ir = mir_module_lookup(mir_unit, stmt->func_decl.function);

            assert(ir->function == stmt->func_decl.function);
            assert(ir->param_count == 1);
        }
    }

    test_context_free(&ctx);
}

int main(void) {
    resolving_a_unit_lowers_every_function();
    a_function_the_unit_never_resolved_is_absent();
    a_lowered_body_carries_the_function_it_came_from();

    printf("mir_unit_test passed\n");

    return 0;
}
