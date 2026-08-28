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
static const Type *make_struct(TestContext *ctx, TypeRegistry *registry, const char *name,
                               const char **fields, const Type **field_types, size_t count) {
    Type *type = type_registry_declare_struct(registry, string_from_cstr(&ctx->strings, name), count);

    for (size_t i = 0; i < count; i++) {
        type_add_field(type, string_from_cstr(&ctx->strings, fields[i]), field_types[i]);
    }

    type_registry_drop_of(registry, type);

    return type;
}

// What a type owns decides its drop function, and a type that owns nothing has
// none: the free path tests for that rather than calling a function to learn
// there is nothing to do.
static void test_a_type_that_owns_nothing_has_no_drop() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"x", "y"};
    const Type *types[] = {int_type, int_type};

    assert(type_registry_drop_of(registry, make_struct(&ctx, registry, "Point", names, types, 2)) == NULL);

    const Type *owning = type_registry_box_to(registry, int_type);
    const Type *borrowing = type_registry_ref_to(registry, int_type);

    assert(type_registry_drop_of(registry, owning) != NULL);
    assert(type_registry_drop_of(registry, borrowing) == NULL);

    // A struct owns through whichever field does, so one owning field is what
    // earns it a drop.
    const char *held[] = {"held"};
    const Type *borrowed_field[] = {borrowing};
    const Type *owned_field[] = {owning};

    assert(type_registry_drop_of(registry,
                                 make_struct(&ctx, registry, "Borrower", held, borrowed_field, 1)) == NULL);
    assert(type_registry_drop_of(registry, make_struct(&ctx, registry, "Owner", held, owned_field, 1)) !=
           NULL);

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

    const Type *characters = type_registry_ptr_to(registry, registry->builtins.byte_type);

    assert(characters->kind == TYPE_PTR);
    assert(type_pointee(characters) == registry->builtins.byte_type);
    assert(type_registry_drop_of(registry, characters) == NULL);

    // One byte, which is the unit the count of an allocation is given in.
    assert(type_registry_size_of(registry, type_pointee(characters)) == 1 &&
           type_registry_align_of(registry, type_pointee(characters)) == 1);

    // Named so a diagnostic can print it, but declared into no scope.
    assert(!test_compiles("func f(b: byte): int { return 0; }\n"));

    test_context_free(&ctx);
}

// One allocation for the header and payload together, and one free for both.
static void test_alloc_and_free_are_one_allocation() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"health"};
    const Type *types[] = {int_type};
    const Type *player = make_struct(&ctx, registry, "Player", names, types, 1);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *p = object_alloc(allocator, type_registry_size_of(registry, player),
                           type_registry_drop_of(registry, player));

    assert(p);
    assert(counts.allocs == 1);
    assert(object_of(p)->drop == type_registry_drop_of(registry, player));

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
    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"a", "b"};
    const Type *types[] = {int_type, int_type};
    const Type *pair = make_struct(&ctx, registry, "Pair", names, types, 2);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *p =
        object_alloc(allocator, type_registry_size_of(registry, pair), type_registry_drop_of(registry, pair));

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
    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *names[] = {"a", "b"};
    const Type *types[] = {int_type, int_type};
    const Type *pair = make_struct(&ctx, registry, "Pair", names, types, 2);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    char *p =
        object_alloc(allocator, type_registry_size_of(registry, pair), type_registry_drop_of(registry, pair));

    for (size_t i = 0; i < type_registry_size_of(registry, pair); i++) {
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
    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *inner_names[] = {"n"};
    const Type *inner_types[] = {int_type};
    const Type *inner = make_struct(&ctx, registry, "Inner", inner_names, inner_types, 1);

    const char *outer_names[] = {"child"};
    const Type *outer_types[] = {type_registry_box_to(registry, inner)};
    const Type *outer = make_struct(&ctx, registry, "Outer", outer_names, outer_types, 1);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *child = object_alloc(allocator, type_registry_size_of(registry, inner),
                               type_registry_drop_of(registry, inner));
    void *parent = object_alloc(allocator, type_registry_size_of(registry, outer),
                                type_registry_drop_of(registry, outer));

    memcpy(parent, &child, sizeof(child));

    assert(counts.allocs == 2);

    object_free(allocator, parent);

    // Both: the parent, and the child its owning field named.
    assert(counts.frees == 2);

    test_context_free(&ctx);
}

// A free reaches an owning field where the layout put it, not where the walk
// happens to start: a struct whose owning field sits behind others is freed
// through that field's offset.
static void test_freeing_reaches_an_owning_field_at_its_offset() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = type_registry_create(ctx.arena, &ctx.strings);
    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *inner_names[] = {"n"};
    const Type *inner_types[] = {int_type};
    const Type *inner = make_struct(&ctx, registry, "Inner", inner_names, inner_types, 1);

    // The owning field is last, so a walk that ignored offsets would read the
    // leading ints as an address.
    const char *outer_names[] = {"a", "b", "child"};
    const Type *outer_types[] = {int_type, int_type, type_registry_box_to(registry, inner)};
    const Type *outer = make_struct(&ctx, registry, "Outer", outer_names, outer_types, 3);

    const TypeField *child_field = type_find_field(outer, string_from_cstr(&ctx.strings, "child"));
    size_t offset = type_registry_layout_of(registry, outer)->offsets[child_field - type_fields(outer)];

    assert(offset > 0);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *child = object_alloc(allocator, type_registry_size_of(registry, inner),
                               type_registry_drop_of(registry, inner));
    void *parent = object_alloc(allocator, type_registry_size_of(registry, outer),
                                type_registry_drop_of(registry, outer));

    memcpy((char *)parent + offset, &child, sizeof(child));

    assert(counts.allocs == 2);

    object_free(allocator, parent);

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
    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const char *inner_names[] = {"n"};
    const Type *inner_types[] = {int_type};
    const Type *inner = make_struct(&ctx, registry, "Inner", inner_names, inner_types, 1);

    const char *outer_names[] = {"borrowed"};
    const Type *outer_types[] = {type_registry_ref_to(registry, inner)};
    const Type *outer = make_struct(&ctx, registry, "Outer", outer_names, outer_types, 1);

    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    void *borrowed = object_alloc(allocator, type_registry_size_of(registry, inner),
                                  type_registry_drop_of(registry, inner));
    void *holder = object_alloc(allocator, type_registry_size_of(registry, outer),
                                type_registry_drop_of(registry, outer));

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

    const Type *owning = registry->builtins.string_type;
    const Type *borrowing = type_registry_ref_to(registry, registry->builtins.str_type);

    // A header carries more than the reference it lends: the block it owns
    // carries a capacity beside the address, where a reference names only the
    // characters and the count.
    assert(type_registry_size_of(registry, owning) > type_registry_size_of(registry, borrowing));

    assert(type_is_owned(owning));
    assert(!type_is_owned(borrowing));

    assert(!type_is_copyable(owning));
    assert(type_is_copyable(borrowing));

    // The characters themselves are held by nothing, so a slot never reserves
    // room for them and every use goes through the reference above.
    assert(!type_is_sized(registry->builtins.str_type));
    assert(type_is_sized(borrowing));

    // What a reference carries is a fact the type is given, while whether a
    // value can be held is what its kind means. So a reference to characters
    // carries a count while the reference itself is held like any other value.
    assert(type_metadata_of(registry->builtins.str_type) == TYPE_META_LENGTH);
    assert(type_metadata_of(borrowing) == TYPE_META_NONE);
    assert(type_metadata_of(owning) == TYPE_META_NONE);
    assert(type_is_sized(owning));

    // An array of an owning element frees each of them, so it carries a drop
    // the same way. An array of ints owns nothing and has none.
    const Type *ints = type_registry_array_of(registry, registry->builtins.int_type, 4);

    assert(type_registry_drop_of(registry, ints) == NULL);
    assert(!type_is_owned(ints));

    const Type *strings = type_registry_array_of(registry, owning, 2);

    assert(type_registry_drop_of(registry, strings) != NULL);
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

    const Type *floats = type_registry_array_of(registry, registry->builtins.float_type, 3);

    assert(type_array_element(floats) == registry->builtins.float_type);
    assert(type_array_length(floats) == 3);

    // The whole run and nothing else: what a C 'float[3]' occupies.
    assert(type_registry_size_of(registry, floats) ==
           type_registry_size_of(registry, registry->builtins.float_type) * 3);
    assert(type_registry_align_of(registry, floats) ==
           type_registry_align_of(registry, registry->builtins.float_type));

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

    const Type *ints = type_registry_array_of(registry, registry->builtins.int_type, 4);

    assert(!type_is_owned(ints));
    assert(type_is_copyable(ints));

    const Type *boxes = type_registry_array_of(registry, type_registry_box_to(registry, ints), 2);

    assert(type_is_owned(boxes));
    assert(!type_is_copyable(boxes));

    // An array of arrays owns through both levels, so the inner element is what
    // the outer one's answer rests on.
    const Type *nested = type_registry_array_of(registry, boxes, 3);

    assert(type_is_owned(nested));

    // Asked of the element rather than of the plan, which is a separate fact
    // held elsewhere: what an array owns is what it holds, not what was derived
    // to free it.
    assert(type_is_owned(type_array_element(nested)));

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
    const Type *int_type = registry->builtins.int_type;

    const Type *box = type_registry_box_to(registry, int_type);

    assert(type_pointee(box) == int_type);
    assert(type_field_count(box) == 0);
    assert(type_fields(box) == NULL);

    Type *player = type_registry_declare_struct(registry, string_from_cstr(&ctx.strings, "Player"), 1);
    type_add_field(player, string_from_cstr(&ctx.strings, "health"), int_type);

    assert(type_field_count(player) == 1);
    assert(type_pointee(player) == NULL);

    const Type *ints = type_registry_array_of(registry, int_type, 3);

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
    const Type *int_type = registry->builtins.int_type;

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

// A builtin is interned like everything else, and its kind is what finds it: one
// kind, one type, so asking for a kind is a lookup rather than a choice between
// types that share one.
static void test_a_builtin_is_interned_and_found_by_its_kind() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    assert(type_registry_get_builtin(registry, TYPE_INT) == registry->builtins.int_type);
    assert(type_registry_get_builtin(registry, TYPE_BOOL) == registry->builtins.bool_type);

    // A byte is not an int of another width: it is its own kind, which is what
    // lets a kind name exactly one type.
    assert(type_registry_get_builtin(registry, TYPE_BYTE) == registry->builtins.byte_type);
    assert(registry->builtins.byte_type != registry->builtins.int_type);
    assert(registry->builtins.byte_type->kind == TYPE_BYTE);

    test_context_free(&ctx);
}

int main(void) {
    test_a_raw_pointer_carries_a_stride_and_drops_nothing();
    test_a_string_owns_and_a_reference_to_one_does_not();
    test_a_type_carries_only_what_its_kind_has();
    test_methods_live_beside_the_type_not_in_it();
    test_a_builtin_is_interned_and_found_by_its_kind();
    test_an_array_is_its_elements_laid_end_to_end();
    test_an_array_owns_exactly_when_its_element_does();
    test_a_type_that_owns_nothing_has_no_drop();
    test_alloc_and_free_are_one_allocation();
    test_the_payload_follows_the_header();
    test_a_fresh_payload_is_zeroed();
    test_freeing_an_object_frees_what_it_owns();
    test_freeing_reaches_an_owning_field_at_its_offset();
    test_freeing_does_not_follow_a_ref_field();
    test_freeing_null_is_a_no_op();

    printf("All object tests passed\n");
    return 0;
}
