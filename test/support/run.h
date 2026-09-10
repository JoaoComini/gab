#ifndef GAB_TEST_RUN_H
#define GAB_TEST_RUN_H

#include "ast/resolve.h"
#include "driver/interface.h"
#include "mir/mir_build.h"
#include "scope.h"
#include "support/test_context.h"
#include "syntax/parser.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The prelude declares methods on the primitives, which only a compilation given permission may do,
 * and declares into the global scope as the compilation reading it does. */
static inline bool test_resolve_ir_with(TestContext *ctx, Scope *into, ASTModule **unit, MIRModule **mir_unit,
                                        ResolvedModule **out, const char *source, bool declares_intrinsics) {
    if (!parse_module((const char *const[]){test_in_a_module(source)}, 1, NULL, ctx->arena, &ctx->strings,
                      unit, &ctx->diagnostics)) {
        return false;
    }

    ResolvedModule *resolved;

    Resolver resolver = test_resolver(ctx, NULL);

    ModulePrivileges privileges = {.intrinsics = declares_intrinsics};

    if (!resolve_module(&resolver, *unit, into, privileges, &resolved)) {
        return false;
    }

    MIRModule *bodies;
    bool built = mir_build(ctx->arena, resolved, NULL, &bodies, &ctx->diagnostics);

    if (mir_unit) {
        *mir_unit = bodies;
    }

    if (out) {
        *out = resolved;
    }

    return built;
}

static inline bool test_resolve_ir(TestContext *ctx, Scope *scope, ASTModule **unit, MIRModule **mir_unit,
                                   ResolvedModule **out, const char *source) {
    return test_resolve_ir_with(ctx, scope, unit, mir_unit, out, source, false);
}

/* Resolves into a module scope under 'global', which is where what the source declares is found. */
static inline Scope *test_resolve(TestContext *ctx, Scope *global, ASTModule **unit, const char *source) {
    ResolvedModule *resolved = NULL;

    if (!test_resolve_ir(ctx, global, unit, NULL, &resolved, source)) {
        return NULL;
    }

    return resolved->declared->scope;
}

/* A global scope the core has declared into, which is what 'len', 'as_bytes' and '[]' resolve through.
 * It is read from the interface a compilation reads, so what a test resolves against is what 'gabc'
 * hands a program rather than a second reading of the core's source. */
static inline Scope *test_scope_with_core(TestContext *ctx, ModuleMap **out_modules) {
    Scope *scope = ctx->global;

    char *core = gab_interface_read(GAB_TEST_CORE_INTERFACE);

    assert(core && "the core is compiled before a test resolves against it");

    ASTModule *unit = ast_module_create(ctx->arena);
    MIRModule *bodies = NULL;
    ResolvedModule *resolved = NULL;

    bool ok = test_resolve_ir_with(ctx, scope, &unit, &bodies, &resolved, core, true);

    assert(ok && "the core compiles");
    (void)ok;

    free(core);

    /* Holds what a test imports; the core is not among them, having declared into 'scope' itself. */
    ModuleMap *modules = module_map_create_alloc(arena_allocator(ctx->arena), 8);

    if (out_modules) {
        *out_modules = modules;
    }

    return scope;
}

static inline bool test_compiles(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create_kind(ctx.arena, ctx.global, SCOPE_MODULE);
    ASTModule *unit;

    bool ok = test_resolve(&ctx, scope, &unit, source);

    test_context_free(&ctx);

    return ok;
}

static inline bool test_diagnostic_mentions(const char *source, const char *needle) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create_kind(ctx.arena, ctx.global, SCOPE_MODULE);
    ASTModule *unit;

    test_resolve(&ctx, scope, &unit, source);

    bool found = false;

    for (size_t i = 0; i < diagnostics_count(&ctx.diagnostics); i++) {
        if (strstr(diagnostics_get(&ctx.diagnostics, i)->message, needle)) {
            found = true;
            break;
        }
    }

    test_context_free(&ctx);

    return found;
}

/* Resolved as the core is, which is the only compilation permitted to declare what the compiler supplies. */
static inline bool test_core_diagnostic_mentions(const char *source, const char *needle) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create_kind(ctx.arena, ctx.global, SCOPE_MODULE);
    ASTModule *unit;
    MIRModule *bodies;
    ResolvedModule *resolved;

    test_resolve_ir_with(&ctx, scope, &unit, &bodies, &resolved, source, true);

    bool found = false;

    for (size_t i = 0; i < diagnostics_count(&ctx.diagnostics); i++) {
        if (strstr(diagnostics_get(&ctx.diagnostics, i)->message, needle)) {
            found = true;
            break;
        }
    }

    test_context_free(&ctx);

    return found;
}

static inline size_t test_diagnostic_count(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create_kind(ctx.arena, ctx.global, SCOPE_MODULE);
    ASTModule *unit;

    test_resolve(&ctx, scope, &unit, source);

    size_t count = diagnostics_count(&ctx.diagnostics);

    test_context_free(&ctx);

    return count;
}

#endif
