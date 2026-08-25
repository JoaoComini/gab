#include "object.h"
#include "support/test_context.h"
#include "type.h"
#include "type_registry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// A free is the whole point, and a freed pointer cannot be asked whether it was
// freed. Counting the allocator's calls is what gives the assertions something
// to see.
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
// out — freeing walks offsets, so the layout has to be real.
static Type *make_struct(TestContext *ctx, const char *name, const char **fields, Type **field_types,
                         size_t count) {
    Type *type = type_struct_create(ctx->arena, string_from_cstr(&ctx->strings, name), count);

    for (size_t i = 0; i < count; i++) {
        type_add_field(type, string_from_cstr(&ctx->strings, fields[i]), field_types[i]);
    }

    type_layout_compute(type);
    object_select_drop(type);

    return type;
}

// What a type owns decides its drop function, and a type that owns nothing has
// none: the free path tests for that rather than calling a function to learn
// there is nothing to do.
static void test_a_type_that_owns_nothing_has_no_drop() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"x", "y"};
    Type *types[] = {int_type, int_type};

    assert(make_struct(&ctx, "Point", names, types, 2)->drop == NULL);

    Type *owning = type_registry_indirect_to_kind(registry, int_type, false);
    Type *borrowing = type_registry_indirect_to_kind(registry, int_type, true);

    assert(owning->drop != NULL);
    assert(borrowing->drop == NULL);

    // A struct owns through whichever field does, so one owning field is what
    // earns it a drop.
    const char *held[] = {"held"};
    Type *borrowed_field[] = {borrowing};
    Type *owned_field[] = {owning};

    assert(make_struct(&ctx, "Borrower", held, borrowed_field, 1)->drop == NULL);
    assert(make_struct(&ctx, "Owner", held, owned_field, 1)->drop != NULL);

    test_context_free(&ctx);
}

// One allocation for the header and payload together, and one free for both.
static void test_alloc_and_free_are_one_allocation() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"health"};
    Type *types[] = {int_type};
    Type *player = make_struct(&ctx, "Player", names, types, 1);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *p = object_alloc(allocator, player);

    assert(p);
    assert(counts.allocs == 1);
    assert(object_of(p)->type == player);

    object_free(allocator, p);

    assert(counts.frees == 1);

    test_context_free(&ctx);
}

// The payload follows the header immediately, which is what makes a 'box T' the
// address of the payload and byte-identical to a stack pointer.
static void test_the_payload_follows_the_header() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"a", "b"};
    Type *types[] = {int_type, int_type};
    Type *pair = make_struct(&ctx, "Pair", names, types, 2);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *p = object_alloc(allocator, pair);

    assert((char *)object_of(p) + sizeof(ObjectHeader) == (char *)p);

    object_free(allocator, p);

    test_context_free(&ctx);
}

// A fresh payload is zeroed, so a pointer field nobody assigned is NULL rather
// than whatever the allocator left behind. Freeing depends on this: it walks
// the pointer fields and has no other way to tell an unset one from a real one.
static void test_a_fresh_payload_is_zeroed() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"a", "b"};
    Type *types[] = {int_type, int_type};
    Type *pair = make_struct(&ctx, "Pair", names, types, 2);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    char *p = object_alloc(allocator, pair);

    for (size_t i = 0; i < pair->size; i++) {
        assert(p[i] == 0);
    }

    object_free(allocator, p);

    test_context_free(&ctx);
}

// Freeing an object frees what its owning fields name, so a tree goes in one
// call. This is the whole reason the header carries a Type.
static void test_freeing_an_object_frees_what_it_owns() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *inner_names[] = {"n"};
    Type *inner_types[] = {int_type};
    Type *inner = make_struct(&ctx, "Inner", inner_names, inner_types, 1);

    const char *outer_names[] = {"child"};
    Type *outer_types[] = {type_registry_indirect_to(registry, inner)};
    Type *outer = make_struct(&ctx, "Outer", outer_names, outer_types, 1);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *child = object_alloc(allocator, inner);
    void *parent = object_alloc(allocator, outer);

    memcpy(parent, &child, sizeof(child));

    assert(counts.allocs == 2);

    object_free(allocator, parent);

    // Both: the parent, and the child its owning field named.
    assert(counts.frees == 2);

    test_context_free(&ctx);
}

// A 'ref T' field names something it does not own, so freeing the holder must
// leave the inner alone. Freeing it here would be a double free the moment
// its real owner went.
static void test_freeing_does_not_follow_a_ref_field() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *inner_names[] = {"n"};
    Type *inner_types[] = {int_type};
    Type *inner = make_struct(&ctx, "Inner", inner_names, inner_types, 1);

    const char *outer_names[] = {"borrowed"};
    Type *outer_types[] = {type_registry_indirect_to_kind(registry, inner, true)};
    Type *outer = make_struct(&ctx, "Outer", outer_names, outer_types, 1);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *borrowed = object_alloc(allocator, inner);
    void *holder = object_alloc(allocator, outer);

    memcpy(holder, &borrowed, sizeof(borrowed));

    object_free(allocator, holder);

    // Only the holder. The borrowed object is still its owner's to free.
    assert(counts.frees == 1);

    object_free(allocator, borrowed);

    assert(counts.frees == 2);

    test_context_free(&ctx);
}

// A NULL 'box T' is what an unassigned pointer field holds, so every free path
// would otherwise need the same guard.
static void test_freeing_null_is_a_no_op() {
    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    object_free(allocator, NULL);

    assert(counts.frees == 0);
}

int main(void) {
    test_a_type_that_owns_nothing_has_no_drop();
    test_alloc_and_free_are_one_allocation();
    test_the_payload_follows_the_header();
    test_a_fresh_payload_is_zeroed();
    test_freeing_an_object_frees_what_it_owns();
    test_freeing_does_not_follow_a_ref_field();
    test_freeing_null_is_a_no_op();

    printf("All object tests passed\n");
    return 0;
}
