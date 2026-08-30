#ifndef GAB_TEST_CONTEXT_H
#define GAB_TEST_CONTEXT_H

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "diagnostics.h"
#include "scope.h"
#include "string/string_pool.h"

#define TEST_ARENA_BLOCK_SIZE 2048

typedef struct {
    Arena *arena;
    StringPool strings;
    Diagnostics diagnostics;
} TestContext;

static inline const char *test_in_a_module(const char *source) {
    static char buffer[1 << 16];

    while (*source == ' ' || *source == '\n' || *source == '\t') {
        source++;
    }

    if (strncmp(source, "module ", 7) == 0) {
        return source;
    }

    int written = snprintf(buffer, sizeof(buffer), "module test;\n%s", source);

    assert(written > 0 && (size_t)written < sizeof(buffer));

    return buffer;
}

static inline void test_context_init(TestContext *ctx) {
    ctx->arena = arena_create(TEST_ARENA_BLOCK_SIZE);
    string_pool_init(&ctx->strings, ctx->arena);
    diagnostics_init(&ctx->diagnostics, ctx->arena, "<test>");
}

static inline const Type *test_named_type(TestContext *ctx, Scope *scope, const char *name) {
    return scope_type_lookup(scope, string_from_cstr(&ctx->strings, name));
}

static inline void test_context_free(TestContext *ctx) {
    diagnostics_free(&ctx->diagnostics);
    string_pool_free(&ctx->strings);
    arena_destroy(ctx->arena);
}

#endif
