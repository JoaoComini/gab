#include "object.h"
#include "support/test_context.h"
#include "type.h"
#include "type_registry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// A free is the whole point of refcounting, and a freed pointer cannot be
// asked whether it was freed. Counting the allocator's calls is what gives the
// assertions something to see.
typedef struct {
    int allocs;
    int frees;
} AllocCounts;

static void *counting_alloc(void *ctx, size_t size) {
    ((AllocCounts *)ctx)->allocs++;
    return malloc(size);
}

static void counting_free(void *ctx, void *ptr) {
    ((AllocCounts *)ctx)->frees++;
    free(ptr);
}

static Allocator counting_allocator(AllocCounts *counts) {
    return (Allocator){.alloc = counting_alloc, .free = counting_free, .ctx = counts};
}

// A struct type with the given fields, laid out as the compiler would lay it
// out — release walks offsets, so the layout has to be real.
static Type *make_struct(TestContext *ctx, TypeRegistry *registry, const char *name, const char **fields,
                         Type **field_types, size_t count) {
    Type *type = type_struct_create(ctx->arena, string_from_cstr(&ctx->strings, name), count);

    for (size_t i = 0; i < count; i++) {
        type_add_field(type, string_from_cstr(&ctx->strings, fields[i]), field_types[i]);
    }

    type_layout_compute(type);

    (void)registry;
    return type;
}

static void test_alloc_starts_at_one_and_frees_at_zero() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"health"};
    Type *types[] = {int_type};
    Type *player = make_struct(&ctx, registry, "Player", names, types, 1);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *p = gab_object_alloc(allocator, player);

    assert(p);
    assert(counts.allocs == 1);
    assert(gab_header_of(p)->strong == 1);
    assert(gab_header_of(p)->weak == 0);
    assert(gab_header_of(p)->type == player);

    gab_object_release(allocator, p);
    assert(counts.frees == 1);

    type_registry_destroy(registry);
    test_context_free(&ctx);
}

// The payload is what a '*T' addresses, and it sits immediately after the
// header at its own alignment.
static void test_payload_follows_the_header() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"a", "b"};
    Type *types[] = {int_type, int_type};
    Type *pair = make_struct(&ctx, registry, "Pair", names, types, 2);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *p = gab_object_alloc(allocator, pair);

    assert((char *)gab_header_of(p) + sizeof(ObjHeader) == (char *)p);
    assert((uintptr_t)p % 8 == 0);

    // Zeroed, so a pointer field nobody set reads as NULL rather than garbage.
    assert(((int *)p)[0] == 0 && ((int *)p)[1] == 0);

    gab_object_release(allocator, p);

    type_registry_destroy(registry);
    test_context_free(&ctx);
}

// Two references, one object: the first release must not free it.
static void test_two_references_keep_it_alive() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"n"};
    Type *types[] = {int_type};
    Type *box = make_struct(&ctx, registry, "Box", names, types, 1);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *p = gab_object_alloc(allocator, box);
    gab_object_retain(p);

    assert(gab_header_of(p)->strong == 2);

    gab_object_release(allocator, p);
    assert(counts.frees == 0);
    assert(gab_header_of(p)->strong == 1);

    gab_object_release(allocator, p);
    assert(counts.frees == 1);

    type_registry_destroy(registry);
    test_context_free(&ctx);
}

// Releasing an object releases what it exclusively owned, which is what makes
// a deterministic free reach a whole tree.
static void test_release_walks_pointer_fields() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *inner_names[] = {"n"};
    Type *inner_types[] = {int_type};
    Type *inner = make_struct(&ctx, registry, "Inner", inner_names, inner_types, 1);

    Type *inner_ptr = type_registry_pointer_to(registry, inner);

    const char *outer_names[] = {"child"};
    Type *outer_types[] = {inner_ptr};
    Type *outer = make_struct(&ctx, registry, "Outer", outer_names, outer_types, 1);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *child = gab_object_alloc(allocator, inner);
    void *parent = gab_object_alloc(allocator, outer);

    // The parent takes ownership of the child, as a store would.
    memcpy((char *)parent, &child, sizeof(child));
    gab_object_retain(child);

    assert(gab_header_of(child)->strong == 2);
    assert(counts.allocs == 2);

    // The creating reference goes; the parent's keeps it alive.
    gab_object_release(allocator, child);
    assert(counts.frees == 0);

    // Releasing the parent frees both, in one call.
    gab_object_release(allocator, parent);
    assert(counts.frees == 2);

    type_registry_destroy(registry);
    test_context_free(&ctx);
}

// An inline struct field is part of this payload, so the pointers inside it are
// released by this object's teardown rather than by a separate one.
static void test_release_walks_into_an_inline_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *leaf_names[] = {"n"};
    Type *leaf_types[] = {int_type};
    Type *leaf = make_struct(&ctx, registry, "Leaf", leaf_names, leaf_types, 1);

    Type *leaf_ptr = type_registry_pointer_to(registry, leaf);

    // Holder has a pointer; Outer embeds a Holder by value.
    const char *holder_names[] = {"ref"};
    Type *holder_types[] = {leaf_ptr};
    Type *holder = make_struct(&ctx, registry, "Holder", holder_names, holder_types, 1);

    const char *outer_names[] = {"pad", "held"};
    Type *outer_types[] = {int_type, holder};
    Type *outer = make_struct(&ctx, registry, "Outer", outer_names, outer_types, 2);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *leaf_obj = gab_object_alloc(allocator, leaf);
    void *outer_obj = gab_object_alloc(allocator, outer);

    size_t held_offset = 0;
    assert(type_field_offset(outer, string_from_cstr(&ctx.strings, "held"), &held_offset));

    memcpy((char *)outer_obj + held_offset, &leaf_obj, sizeof(leaf_obj));
    gab_object_retain(leaf_obj);

    gab_object_release(allocator, leaf_obj);
    assert(counts.frees == 0);

    gab_object_release(allocator, outer_obj);
    assert(counts.frees == 2);

    type_registry_destroy(registry);
    test_context_free(&ctx);
}

// Every release path would otherwise need the same guard, since an unassigned
// '*T' is NULL.
static void test_null_is_tolerated() {
    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    gab_object_retain(NULL);
    gab_object_release(allocator, NULL);

    assert(counts.frees == 0);
}

int main(void) {
    test_alloc_starts_at_one_and_frees_at_zero();
    test_payload_follows_the_header();
    test_two_references_keep_it_alive();
    test_release_walks_pointer_fields();
    test_release_walks_into_an_inline_struct();
    test_null_is_tolerated();

    printf("All object tests passed\n");
    return 0;
}
