#include "object.h"
#include "support/run.h"
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
static TypeHandle make_struct(TestContext *ctx, TypeRegistry *registry, const char *name, const char **fields,
                              TypeHandle *field_types, size_t count) {
    Type *type = type_registry_declare_struct(registry, NULL, string_from_cstr(&ctx->strings, name), count);

    for (size_t i = 0; i < count; i++) {
        type_add_field(type, string_from_cstr(&ctx->strings, fields[i]), field_types[i]);
    }

    type_registry_finish_struct(registry, type);

    return type;
}

// What a type owns decides its drop function, and a type that owns nothing has
// none: the free path tests for that rather than calling a function to learn
// there is nothing to do.
static void test_a_type_that_owns_nothing_has_no_drop() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    TypeHandle int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"x", "y"};
    TypeHandle types[] = {int_type, int_type};

    assert(make_struct(&ctx, registry, "Point", names, types, 2)->drop == NULL);

    TypeHandle owning = type_registry_box_to(registry, int_type);
    TypeHandle borrowing = type_registry_ref_to(registry, int_type);

    assert(owning->drop != NULL);
    assert(borrowing->drop == NULL);

    // A struct owns through whichever field does, so one owning field is what
    // earns it a drop.
    const char *held[] = {"held"};
    TypeHandle borrowed_field[] = {borrowing};
    TypeHandle owned_field[] = {owning};

    assert(make_struct(&ctx, registry, "Borrower", held, borrowed_field, 1)->drop == NULL);
    assert(make_struct(&ctx, registry, "Owner", held, owned_field, 1)->drop != NULL);

    test_context_free(&ctx);
}

// A 'ptr T' carries what it points at, so a walk over a block knows its stride
// without asking the header naming it. It still drops nothing: how many
// elements are live is the count on that header, so freeing them is its
// business rather than the pointer's.
static void test_a_raw_pointer_carries_a_stride_and_drops_nothing() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    TypeHandle characters = type_registry_ptr_to(registry, registry->builtins.byte_type);

    assert(characters->kind == TYPE_PTR);
    assert(type_pointee(characters) == registry->builtins.byte_type);
    assert(characters->drop == NULL);

    // One byte, which is the unit the count of an allocation is given in.
    assert(type_pointee(characters)->size == 1 && type_pointee(characters)->alignment == 1);

    // Named so a diagnostic can print it, but declared into no scope.
    assert(!test_compiles("func f(b: byte): int { return 0; }\n"));

    test_context_free(&ctx);
}

// One allocation for the header and payload together, and one free for both.
static void test_alloc_and_free_are_one_allocation() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    TypeHandle int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"health"};
    TypeHandle types[] = {int_type};
    TypeHandle player = make_struct(&ctx, registry, "Player", names, types, 1);

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
    TypeHandle int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"a", "b"};
    TypeHandle types[] = {int_type, int_type};
    TypeHandle pair = make_struct(&ctx, registry, "Pair", names, types, 2);

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
    TypeHandle int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"a", "b"};
    TypeHandle types[] = {int_type, int_type};
    TypeHandle pair = make_struct(&ctx, registry, "Pair", names, types, 2);

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
    TypeHandle int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *inner_names[] = {"n"};
    TypeHandle inner_types[] = {int_type};
    TypeHandle inner = make_struct(&ctx, registry, "Inner", inner_names, inner_types, 1);

    const char *outer_names[] = {"child"};
    TypeHandle outer_types[] = {type_registry_box_to(registry, inner)};
    TypeHandle outer = make_struct(&ctx, registry, "Outer", outer_names, outer_types, 1);

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
    TypeHandle int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *inner_names[] = {"n"};
    TypeHandle inner_types[] = {int_type};
    TypeHandle inner = make_struct(&ctx, registry, "Inner", inner_names, inner_types, 1);

    const char *outer_names[] = {"borrowed"};
    TypeHandle outer_types[] = {type_registry_ref_to(registry, inner)};
    TypeHandle outer = make_struct(&ctx, registry, "Outer", outer_names, outer_types, 1);

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

// A string owns its characters and a reference to them owns nothing, which the
// kinds say on their own: neither answer is read off the glue that frees.
static void test_a_string_owns_and_a_reference_to_one_does_not() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    TypeHandle owning = registry->builtins.string_type;
    TypeHandle borrowing = type_registry_ref_to(registry, registry->builtins.str_type);

    // A reference carrying a count is as wide as the header it borrows from:
    // the same address and the same length, differing only in who frees them.
    assert(owning->size == borrowing->size);

    assert(type_is_owned(owning));
    assert(!type_is_owned(borrowing));

    assert(!type_is_copyable(owning));
    assert(type_is_copyable(borrowing));

    // The characters themselves are held by nothing, so a slot never reserves
    // room for them and every use goes through the reference above.
    assert(!type_is_sized(registry->builtins.str_type));
    assert(type_is_sized(borrowing));

    // Two facts a type carries, not one read off the other. Each answers
    // independently of the other, so a type that is unsized without wanting a
    // count carried -- or sized while wanting one -- is expressible rather than
    // being decided by whichever switch was reached.
    assert(type_metadata_of(registry->builtins.str_type) == TYPE_META_LENGTH);
    assert(type_metadata_of(borrowing) == TYPE_META_NONE);
    assert(type_metadata_of(owning) == TYPE_META_NONE);
    assert(type_is_sized(owning));

    // Built past the registry deliberately: this asserts on what a type carries
    // rather than on what a program can name, and the two questions are asked
    // of different things.
    Type *probe = type_create(ctx.arena, TYPE_STRUCT, NULL);

    assert(type_is_sized(probe));
    assert(type_metadata_of(probe) == TYPE_META_NONE);

    probe->sized = false;

    assert(!type_is_sized(probe));
    assert(type_metadata_of(probe) == TYPE_META_NONE);

    // An array of an owning element frees each of them, so it carries a drop
    // the same way. An array of ints owns nothing and has none.
    TypeHandle ints = type_registry_array_of(registry, registry->builtins.int_type, 4);

    assert(ints->drop == NULL);
    assert(!type_is_owned(ints));

    TypeHandle strings = type_registry_array_of(registry, owning, 2);

    assert(strings->drop != NULL);
    assert(type_is_owned(strings));

    test_context_free(&ctx);
}

// An array's element and its length are what its type was applied to, so they
// are read from one place rather than recovered from the layout they produced.
static void test_an_array_is_its_elements_laid_end_to_end() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    TypeHandle floats = type_registry_array_of(registry, registry->builtins.float_type, 3);

    assert(type_array_element(floats) == registry->builtins.float_type);
    assert(type_array_length(floats) == 3);

    // The whole run and nothing else: what a C 'float[3]' occupies.
    assert(floats->size == registry->builtins.float_type->size * 3);
    assert(floats->alignment == registry->builtins.float_type->alignment);

    // Interned on both arguments, so a second length is a second type.
    assert(type_registry_array_of(registry, registry->builtins.float_type, 3) == floats);
    assert(type_registry_array_of(registry, registry->builtins.float_type, 4) != floats);

    test_context_free(&ctx);
}

// An array owns exactly when its element does, which the type says without
// consulting the glue that frees it: the element is what the array was applied
// to, so the question is answered where every other ownership question is.
static void test_an_array_owns_exactly_when_its_element_does() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    TypeHandle ints = type_registry_array_of(registry, registry->builtins.int_type, 4);

    assert(!type_is_owned(ints));
    assert(type_is_copyable(ints));

    TypeHandle boxes = type_registry_array_of(registry, type_registry_box_to(registry, ints), 2);

    assert(type_is_owned(boxes));
    assert(!type_is_copyable(boxes));

    // An array of arrays owns through both levels, so the inner element is what
    // the outer one's answer rests on.
    TypeHandle nested = type_registry_array_of(registry, boxes, 3);

    assert(type_is_owned(nested));

    // Asked of the element rather than of the glue: clearing the drop leaves the
    // answer where it was, since what an array owns is what it holds and not
    // which function was chosen to free it.
    //
    // Reaching past the handle to do it, which is what a test asserting on the
    // glue rather than through it has to do.
    Type *poke = (Type *)nested;
    DropFn glue = poke->drop;

    poke->drop = NULL;

    assert(type_is_owned(nested));

    poke->drop = glue;

    test_context_free(&ctx);
}

// A type carries what its kind gives it and nothing another kind would give, so
// asking a struct what it points at or an indirection what fields it has is
// answered by the absence rather than by whatever sat in the same bytes.
static void test_a_type_carries_only_what_its_kind_has() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;
    TypeHandle int_type = registry->builtins.int_type;

    TypeHandle box = type_registry_box_to(registry, int_type);

    assert(type_pointee(box) == int_type);
    assert(type_field_count(box) == 0);
    assert(type_fields(box) == NULL);

    Type *player = type_registry_declare_struct(registry, NULL, string_from_cstr(&ctx.strings, "Player"), 1);
    type_add_field(player, string_from_cstr(&ctx.strings, "health"), int_type);
    type_registry_finish_struct(registry, player);

    assert(type_field_count(player) == 1);
    assert(type_pointee(player) == NULL);

    TypeHandle ints = type_registry_array_of(registry, int_type, 3);

    assert(type_array_element(ints) == int_type);
    assert(type_array_length(ints) == 3);
    assert(type_pointee(ints) == NULL);
    assert(type_field_count(ints) == 0);

    // A scalar has no payload of any kind, and each question says so rather
    // than reading a field another kind would have filled.
    assert(type_pointee(int_type) == NULL);
    assert(type_field_count(int_type) == 0);

    test_context_free(&ctx);
}

// A type's methods are not part of the type. What may be called on one is
// declared as a program is read -- by a later statement, a later unit, or the
// host before any of them -- while what a type is was settled when it was
// interned, so the two are kept apart and the set is looked up beside it.
static void test_methods_live_beside_the_type_not_in_it() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;
    TypeHandle int_type = registry->builtins.int_type;

    String *name = string_from_cstr(&ctx.strings, "twice");

    assert(type_registry_find_method(registry, int_type, name) == NULL);

    Symbol method = {0};

    assert(type_registry_add_method(registry, int_type, name, &method));
    assert(type_registry_find_method(registry, int_type, name) == &method);

    // Declared once: a second of the same name on the same type is refused
    // rather than replacing what is there.
    assert(!type_registry_add_method(registry, int_type, name, &method));

    // Another type is unaffected, since the set is keyed by which type it is
    // declared on.
    assert(type_registry_find_method(registry, registry->builtins.bool_type, name) == NULL);

    test_context_free(&ctx);
}

// Every type a program can name comes from the registry, nominal ones included:
// a struct is declared through it rather than built beside it, so there is no
// route by which a type it never saw reaches a handle.
static void test_every_type_comes_from_the_registry() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;
    String *name = string_from_cstr(&ctx.strings, "Player");

    Type *player = type_registry_declare_struct(registry, &scope, name, 1);

    type_add_field(player, string_from_cstr(&ctx.strings, "health"), registry->builtins.int_type);
    type_registry_finish_struct(registry, player);

    // Its identity is the declaration, not its shape: the same scope and name
    // find it again, and a second scope declaring the same shape is a second
    // type.
    assert(type_registry_find_struct(registry, &scope, name) == player);

    Scope other;
    scope_init(&other, ctx.arena, &ctx.strings, NULL);

    assert(type_registry_find_struct(registry, &other, name) == NULL);

    test_context_free(&ctx);
}

int main(void) {
    test_a_raw_pointer_carries_a_stride_and_drops_nothing();
    test_a_string_owns_and_a_reference_to_one_does_not();
    test_a_type_carries_only_what_its_kind_has();
    test_methods_live_beside_the_type_not_in_it();
    test_every_type_comes_from_the_registry();
    test_an_array_is_its_elements_laid_end_to_end();
    test_an_array_owns_exactly_when_its_element_does();
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
