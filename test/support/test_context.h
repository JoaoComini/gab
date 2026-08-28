#ifndef GAB_TEST_CONTEXT_H
#define GAB_TEST_CONTEXT_H

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "builtin/builtin.h"
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

// A scope over a registry the standard library has registered into, which is
// what a VM builds. For a test that names 'String' or 'Vec<T>' without running
// one: those are registered types, so a bare scope has never heard of them.
static inline void test_scope_with_library(TestContext *ctx, Scope *scope) {
    TypeRegistry *registry = type_registry_create(ctx->arena, &ctx->strings);

    builtin_register_types(registry);

    scope_init_over(scope, ctx->arena, &ctx->strings, registry);
}

// The type a standard library declared under 'name', or NULL. What a test
// reaches for when it means the 'String' a VM would have provided.
static inline const Type *test_declared_type(TypeRegistry *registry, const char *name) {
    size_t count = 0;
    const Type *const *declared = type_registry_declared(registry, &count);

    for (size_t i = 0; i < count; i++) {
        if (strcmp(declared[i]->name->data, name) == 0) {
            return declared[i];
        }
    }

    return NULL;
}

// Mirrors vm_free's ordering: the pool releases its buckets before the arena
// holding the payloads goes away.
static inline void test_context_free(TestContext *ctx) {
    diagnostics_free(&ctx->diagnostics);
    string_pool_free(&ctx->strings);
    arena_destroy(ctx->arena);
}

#endif
