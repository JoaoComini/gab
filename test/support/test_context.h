#ifndef GAB_TEST_CONTEXT_H
#define GAB_TEST_CONTEXT_H

#include "arena.h"
#include "diagnostics.h"
#include "scope.h"
#include "string/string_pool.h"

#define TEST_ARENA_BLOCK_SIZE 2048

// An arena, string pool, and diagnostics sink bundled with the lifetimes the
// compiler expects. Deliberately not a singleton: tests that need two
// independent pools (to prove they no longer share state) create two of these.
typedef struct {
    Arena *arena;
    StringPool strings;
    Diagnostics diagnostics;
} TestContext;

static inline void test_context_init(TestContext *ctx) {
    ctx->arena = arena_create(TEST_ARENA_BLOCK_SIZE);
    string_pool_init(&ctx->strings, ctx->arena);
    diagnostics_init(&ctx->diagnostics, ctx->arena, "<test>");
}

// Mirrors vm_free's ordering: the pool releases its buckets before the arena
// holding the payloads goes away.
static inline void test_context_free(TestContext *ctx) {
    diagnostics_free(&ctx->diagnostics);
    string_pool_free(&ctx->strings);
    arena_destroy(ctx->arena);
}

#endif
