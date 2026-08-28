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

// An arena, string pool, and diagnostics sink bundled with the lifetimes the
// compiler expects. Deliberately not a singleton: tests that need two
// independent pools (to prove they no longer share state) create two of these.
typedef struct {
    Arena *arena;
    StringPool strings;
    Diagnostics diagnostics;
} TestContext;

// Every unit names a module, and a test snippet is a unit. Applied by the
// helpers rather than written at the top of several hundred string literals:
// which module a snippet declares into is never what the snippet is about.
//
// Source that already names one is returned unchanged, so a test whose subject
// is modules is not given a second directive.
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

// The type named 'name' in a scope, which is how any name is resolved. For a
// test that means the 'String' a VM provides: it is named in that VM's global
// scope like any other type, so this is the lookup a script's own spec does.
static inline const Type *test_named_type(TestContext *ctx, Scope *scope, const char *name) {
    return scope_type_lookup(scope, string_from_cstr(&ctx->strings, name));
}

// Mirrors vm_free's ordering: the pool releases its buckets before the arena
// holding the payloads goes away.
static inline void test_context_free(TestContext *ctx) {
    diagnostics_free(&ctx->diagnostics);
    string_pool_free(&ctx->strings);
    arena_destroy(ctx->arena);
}

#endif
