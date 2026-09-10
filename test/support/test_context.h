#ifndef GAB_TEST_CONTEXT_H
#define GAB_TEST_CONTEXT_H

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ast/resolve.h"
#include "diagnostics.h"
#include "function_registry.h"
#include "memory/arena.h"
#include "scope.h"
#include "string/string_pool.h"

#define TEST_ARENA_BLOCK_SIZE 2048

typedef struct {
    Arena *arena;
    StringPool strings;
    Diagnostics diagnostics;

    /* What a compilation shares, built as 'gab_compile' builds it. */
    TypeRegistry *types;
    FunctionRegistry *functions;

    Scope *global;
} TestContext;

static inline const char *test_in_a_module(const char *source) {
    static char buffer[1 << 16];

    while (*source == ' ' || *source == '\n' || *source == '\t') {
        source++;
    }

    if (strncmp(source, "module ", 7) == 0) {
        return source;
    }

    /* A generated 'module' must precede the imports the source opens with. */
    const char *imports = source;
    size_t import_length = 0;

    while (strncmp(source + import_length, "import ", 7) == 0) {
        const char *end = strchr(source + import_length, '\n');

        if (!end) {
            break;
        }

        import_length = (size_t)(end + 1 - source);
    }

    source += import_length;

    int written =
        snprintf(buffer, sizeof(buffer), "module test;\n%.*s%s", (int)import_length, imports, source);

    assert(written > 0 && (size_t)written < sizeof(buffer));

    return buffer;
}

static inline void test_context_init(TestContext *ctx) {
    ctx->arena = arena_create(TEST_ARENA_BLOCK_SIZE);
    string_pool_init(&ctx->strings, ctx->arena);
    diagnostics_init(&ctx->diagnostics, ctx->arena, "<test>");

    const KnownNames names = known_names(&ctx->strings);

    ctx->types = type_registry_create(ctx->arena, &names);
    ctx->functions = function_registry_create(ctx->arena, ctx->types);
    ctx->global = global_scope_create(ctx->arena, ctx->types);
}

/* A second compilation over the same string pool, as the writer and the reader of an interface are:
 * an id compares by the pointers interning produced, and nothing else may be shared between them. */
static inline TestContext test_context_reading(const TestContext *ctx) {
    TestContext reading = *ctx;

    const KnownNames names = known_names(&reading.strings);

    reading.types = type_registry_create(reading.arena, &names);
    reading.functions = function_registry_create(reading.arena, reading.types);
    reading.global = global_scope_create(reading.arena, reading.types);

    return reading;
}

/* The compilation-wide state a test resolves with, which every module in it shares. */
static inline Resolver test_resolver(TestContext *ctx, ModuleMap *modules) {
    return (Resolver){
        .arena = ctx->arena,
        .strings = &ctx->strings,
        .types = ctx->types,
        .functions = ctx->functions,
        .global = ctx->global,
        .modules = modules,
        .diagnostics = &ctx->diagnostics,
    };
}

static inline const Type *test_named_type(TestContext *ctx, Scope *scope, const char *name) {
    return scope_type_lookup(ctx->types, scope, string_from_cstr(&ctx->strings, name));
}

static inline void test_context_free(TestContext *ctx) {
    diagnostics_free(&ctx->diagnostics);
    string_pool_free(&ctx->strings);
    arena_destroy(ctx->arena);
}

#endif
