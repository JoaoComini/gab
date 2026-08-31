#include "object.h"
#include "support/run.h"
#include "support/test_context.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

static const Type *make_struct(TestContext *ctx, TypeRegistry *registry, const char *name,
                               const char **fields, const Type **field_types, size_t count) {
    TypeFieldSpec spec[8];

    assert(count <= sizeof(spec) / sizeof(*spec));

    for (size_t i = 0; i < count; i++) {
        spec[i] = (TypeFieldSpec){.name = string_from_cstr(&ctx->strings, fields[i]), .type = field_types[i]};
    }

    return type_registry_declare_struct(registry, string_from_cstr(&ctx->strings, name), spec, count);
}

static void test_a_type_that_owns_nothing_has_no_drop() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames primitive_names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &primitive_names);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    const char *names[] = {"x", "y"};
    const Type *types[] = {int_type, int_type};

    assert(type_registry_drop_of(registry, make_struct(&ctx, registry, "Point", names, types, 2)) == NULL);

    const Type *owning = type_registry_box_to(registry, int_type);
    const Type *borrowing = type_registry_ref_to(registry, int_type);

    assert(type_registry_drop_of(registry, owning) != NULL);
    assert(type_registry_drop_of(registry, borrowing) == NULL);

    const char *held[] = {"held"};
    const Type *borrowed_field[] = {borrowing};
    const Type *owned_field[] = {owning};

    assert(type_registry_drop_of(registry,
                                 make_struct(&ctx, registry, "Borrower", held, borrowed_field, 1)) == NULL);
    assert(type_registry_drop_of(registry, make_struct(&ctx, registry, "Owner", held, owned_field, 1)) !=
           NULL);

    test_context_free(&ctx);
}

static void test_a_raw_pointer_carries_a_stride_and_drops_nothing() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    const Type *characters = type_registry_ptr_to(registry, type_registry_get_primitive(registry, TYPE_BYTE));

    assert(type_kind(characters) == TYPE_PTR);
    assert(type_pointee(characters) == type_registry_get_primitive(registry, TYPE_BYTE));
    assert(type_registry_drop_of(registry, characters) == NULL);

    assert(type_registry_size_of(registry, type_pointee(characters)) == 1 &&
           type_registry_align_of(registry, type_pointee(characters)) == 1);

    assert(!test_compiles("func f(b: byte): int { return 0; }\n"));

    test_context_free(&ctx);
}

static void test_alloc_and_free_are_one_allocation() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames primitive_names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &primitive_names);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

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

static void test_the_payload_follows_the_header() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames primitive_names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &primitive_names);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

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

static void test_a_fresh_payload_is_zeroed() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames primitive_names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &primitive_names);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

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

static void test_freeing_an_object_frees_what_it_owns() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames primitive_names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &primitive_names);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

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

    assert(counts.frees == 2);

    test_context_free(&ctx);
}

static void test_freeing_reaches_an_owning_field_at_its_offset() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames primitive_names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &primitive_names);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    const char *inner_names[] = {"n"};
    const Type *inner_types[] = {int_type};
    const Type *inner = make_struct(&ctx, registry, "Inner", inner_names, inner_types, 1);

    const char *outer_names[] = {"a", "b", "child"};
    const Type *outer_types[] = {int_type, int_type, type_registry_box_to(registry, inner)};
    const Type *outer = make_struct(&ctx, registry, "Outer", outer_names, outer_types, 3);

    const TypeField *child_field =
        type_registry_find_field(registry, outer, string_from_cstr(&ctx.strings, "child"));
    size_t offset = type_registry_layout_of(registry, outer)
                        ->offsets[child_field - type_registry_fields_of(registry, outer)->fields];

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

static void test_freeing_does_not_follow_a_ref_field() {
    TestContext ctx;
    test_context_init(&ctx);

    const TypePrimitiveNames primitive_names = type_primitive_names(&ctx.strings);
    TypeRegistry *registry = type_registry_create(ctx.arena, &primitive_names);
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

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

    assert(counts.frees == 1);

    object_free(allocator, borrowed);

    assert(counts.frees == 2);

    test_context_free(&ctx);
}

static void test_freeing_null_is_a_no_op() {
    AllocCounts counts = {0};
    Allocator allocator = counting_allocator(&counts);

    object_free(allocator, NULL);

    assert(counts.frees == 0);
}

static void test_a_string_owns_and_a_reference_to_one_does_not() {
    VM *vm = vm_create();

    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = &vm->env.global_scope;
    TypeRegistry *registry = scope->type_registry;

    const Type *owning = scope_type_lookup(scope, string_from_cstr(&vm->env.strings, "String"));
    const Type *borrowing = type_registry_ref_to(registry, type_registry_get_primitive(registry, TYPE_STR));

    assert(owning);

    assert(type_registry_size_of(registry, owning) == type_registry_size_of(registry, borrowing));

    assert(type_registry_owns(registry, owning));
    assert(!type_registry_owns(registry, borrowing));

    assert(!type_registry_copies(registry, owning));
    assert(type_registry_copies(registry, borrowing));

    assert(!type_is_sized(type_registry_get_primitive(registry, TYPE_STR)));
    assert(type_is_sized(borrowing));

    assert(type_metadata_of(type_registry_get_primitive(registry, TYPE_STR)) == TYPE_META_LENGTH);
    assert(type_metadata_of(borrowing) == TYPE_META_NONE);
    assert(type_metadata_of(owning) == TYPE_META_NONE);
    assert(type_is_sized(owning));

    const Type *ints = type_registry_array_of(registry, type_registry_get_primitive(registry, TYPE_INT), 4);

    assert(type_registry_drop_of(registry, ints) == NULL);
    assert(!type_registry_owns(registry, ints));

    const Type *strings = type_registry_array_of(registry, owning, 2);

    assert(type_registry_drop_of(registry, strings) != NULL);
    assert(type_registry_owns(registry, strings));

    test_context_free(&ctx);
    vm_free(vm);
}

static void test_an_array_is_its_elements_laid_end_to_end() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    const Type *floats =
        type_registry_array_of(registry, type_registry_get_primitive(registry, TYPE_FLOAT), 3);

    assert(type_array_element(floats) == type_registry_get_primitive(registry, TYPE_FLOAT));
    assert(type_array_length(floats) == 3);

    assert(type_registry_size_of(registry, floats) ==
           type_registry_size_of(registry, type_registry_get_primitive(registry, TYPE_FLOAT)) * 3);
    assert(type_registry_align_of(registry, floats) ==
           type_registry_align_of(registry, type_registry_get_primitive(registry, TYPE_FLOAT)));

    assert(type_registry_array_of(registry, type_registry_get_primitive(registry, TYPE_FLOAT), 3) == floats);
    assert(type_registry_array_of(registry, type_registry_get_primitive(registry, TYPE_FLOAT), 4) != floats);

    test_context_free(&ctx);
}

static void test_an_array_owns_exactly_when_its_element_does() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    const Type *ints = type_registry_array_of(registry, type_registry_get_primitive(registry, TYPE_INT), 4);

    assert(!type_registry_owns(registry, ints));
    assert(type_registry_copies(registry, ints));

    const Type *boxes = type_registry_array_of(registry, type_registry_box_to(registry, ints), 2);

    assert(type_registry_owns(registry, boxes));
    assert(!type_registry_copies(registry, boxes));

    const Type *nested = type_registry_array_of(registry, boxes, 3);

    assert(type_registry_owns(registry, nested));

    assert(type_registry_owns(registry, type_array_element(nested)));

    test_context_free(&ctx);
}

static void test_a_type_carries_only_what_its_kind_has() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    const Type *box = type_registry_box_to(registry, int_type);

    assert(type_pointee(box) == int_type);
    assert(type_registry_fields_of(registry, box)->count == 0);
    assert(type_registry_fields_of(registry, box)->fields == NULL);

    const TypeFieldSpec health = {.name = string_from_cstr(&ctx.strings, "health"), .type = int_type};

    const Type *player =
        type_registry_declare_struct(registry, string_from_cstr(&ctx.strings, "Player"), &health, 1);

    assert(type_registry_fields_of(registry, player)->count == 1);
    assert(type_pointee(player) == NULL);

    const Type *ints = type_registry_array_of(registry, int_type, 3);

    assert(type_array_element(ints) == int_type);
    assert(type_array_length(ints) == 3);
    assert(type_pointee(ints) == NULL);
    assert(type_registry_fields_of(registry, ints)->count == 0);

    assert(type_pointee(int_type) == NULL);
    assert(type_registry_fields_of(registry, int_type)->count == 0);

    test_context_free(&ctx);
}

static void test_methods_live_beside_the_type_not_in_it() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;
    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);

    String *name = string_from_cstr(&ctx.strings, "twice");

    assert(type_registry_find_method(registry, int_type, name) == NULL);

    Function method = {.name = name};

    assert(type_registry_declare_method(registry, int_type, &method));

    Function *found = type_registry_find_method(registry, int_type, name);

    assert(found && found->name == name);

    assert(!type_registry_declare_method(registry, int_type, &method));

    assert(type_registry_find_method(registry, type_registry_get_primitive(registry, TYPE_BOOL), name) ==
           NULL);

    test_context_free(&ctx);
}

static void test_a_builtin_is_interned_and_found_by_its_kind() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope scope;
    scope_init(&scope, ctx.arena, &ctx.strings, NULL);

    TypeRegistry *registry = scope.type_registry;

    assert(type_registry_get_primitive(registry, TYPE_INT) ==
           type_registry_get_primitive(registry, TYPE_INT));
    assert(type_registry_get_primitive(registry, TYPE_BOOL) ==
           type_registry_get_primitive(registry, TYPE_BOOL));

    assert(type_registry_get_primitive(registry, TYPE_BYTE) ==
           type_registry_get_primitive(registry, TYPE_BYTE));
    assert(type_registry_get_primitive(registry, TYPE_BYTE) !=
           type_registry_get_primitive(registry, TYPE_INT));
    assert(type_kind(type_registry_get_primitive(registry, TYPE_BYTE)) == TYPE_BYTE);

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
