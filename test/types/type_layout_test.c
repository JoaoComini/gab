#include "ast/resolve.h"
#include "support/test_context.h"
#include "syntax/parser.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    float x, y, z;
} Vec3C;

typedef struct {
    _Bool flag;
    int32_t value;
} LeadingPadC;

typedef struct {
    int32_t value;
    _Bool flag;
} TrailingPadC;

typedef struct {
    _Bool flag;
    Vec3C position;
} NestedC;

typedef struct {
    int32_t only;
} SingleC;

typedef struct {
    void *b;
    int32_t tag;
} BoxRingC;

static const Type *resolve_struct(TestContext *ctx, const char *source, const char *name,
                                  TypeRegistry **out_registry) {
    ASTModule *unit;

    Resolver resolver = test_resolver(ctx, NULL);

    Scope *module_scope = scope_create_kind(ctx->arena, ctx->global, SCOPE_MODULE);

    if (out_registry) {
        *out_registry = ctx->types;
    }

    ResolvedModule *resolved = NULL;

    if (parse_module((const char *const[]){test_in_a_module(source)}, 1, NULL, ctx->arena, &ctx->strings,
                     &unit, &ctx->diagnostics)) {
        resolve_module(&resolver, unit, module_scope, (ModulePrivileges){0}, &resolved);
    }

    if (diagnostics_has_errors(&ctx->diagnostics)) {
        diagnostics_print(&ctx->diagnostics, stderr);
    }

    assert(!diagnostics_has_errors(&ctx->diagnostics));

    return scope_type_lookup(ctx->types, resolved->declared->scope, string_from_cstr(&ctx->strings, name));
}

static size_t offset_of(TestContext *ctx, TypeRegistry *registry, const Type *type, const char *field) {
    const TypeField *found = type_registry_find_field(registry, type, string_from_cstr(&ctx->strings, field));

    assert(found);

    return type_registry_layout_of(registry, type)
        ->offsets[found - type_registry_fields_of(registry, type)->fields];
}

static void test_homogeneous_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Vec3 { x: f32, y: f32, z: f32 }", "Vec3", &registry);

    assert(type != NULL);
    assert(type_kind(type) == TYPE_STRUCT);
    assert(type_registry_fields_of(registry, type)->count == 3);

    assert(type_registry_size_of(registry, type) == sizeof(Vec3C));
    assert(type_registry_align_of(registry, type) == _Alignof(Vec3C));

    assert(offset_of(&ctx, registry, type, "x") == offsetof(Vec3C, x));
    assert(offset_of(&ctx, registry, type, "y") == offsetof(Vec3C, y));
    assert(offset_of(&ctx, registry, type, "z") == offsetof(Vec3C, z));

    test_context_free(&ctx);
}

static void test_interior_padding() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Pad { flag: bool, value: i32 }", "Pad", &registry);

    assert(type_registry_size_of(registry, type) == sizeof(LeadingPadC));
    assert(type_registry_align_of(registry, type) == _Alignof(LeadingPadC));

    assert(offset_of(&ctx, registry, type, "flag") == offsetof(LeadingPadC, flag));
    assert(offset_of(&ctx, registry, type, "value") == offsetof(LeadingPadC, value));

    test_context_free(&ctx);
}

static void test_trailing_padding() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Pad { value: i32, flag: bool }", "Pad", &registry);

    assert(type_registry_size_of(registry, type) == sizeof(TrailingPadC));
    assert(offset_of(&ctx, registry, type, "value") == offsetof(TrailingPadC, value));
    assert(offset_of(&ctx, registry, type, "flag") == offsetof(TrailingPadC, flag));

    test_context_free(&ctx);
}

static void test_nested_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx,
                                      "struct Vec3 { x: f32, y: f32, z: f32 }"
                                      "struct Nested { flag: bool, position: Vec3 }",
                                      "Nested", &registry);

    assert(type_registry_size_of(registry, type) == sizeof(NestedC));
    assert(type_registry_align_of(registry, type) == _Alignof(NestedC));

    assert(offset_of(&ctx, registry, type, "flag") == offsetof(NestedC, flag));
    assert(offset_of(&ctx, registry, type, "position") == offsetof(NestedC, position));

    const TypeField *position =
        type_registry_find_field(registry, type, string_from_cstr(&ctx.strings, "position"));
    assert(type_kind(position->type) == TYPE_STRUCT);
    assert(type_registry_size_of(registry, position->type) == sizeof(Vec3C));

    test_context_free(&ctx);
}

static void test_single_field_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Single { only: i32 }", "Single", &registry);

    assert(type_registry_size_of(registry, type) == sizeof(SingleC));
    assert(type_registry_align_of(registry, type) == _Alignof(SingleC));
    assert(offset_of(&ctx, registry, type, "only") == offsetof(SingleC, only));

    test_context_free(&ctx);
}

static void test_empty_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Empty { }", "Empty", &registry);

    assert(type != NULL);
    assert(type_registry_fields_of(registry, type)->count == 0);
    assert(type_registry_size_of(registry, type) == 0);
    assert(type_registry_align_of(registry, type) == 1);

    test_context_free(&ctx);
}

static void test_trailing_comma_allowed() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Vec3 { x: f32, y: f32, z: f32, }", "Vec3", &registry);

    assert(type_registry_fields_of(registry, type)->count == 3);
    assert(type_registry_size_of(registry, type) == sizeof(Vec3C));

    test_context_free(&ctx);
}

static void test_an_unknown_field_type_is_reported_once() {
    TestContext ctx;
    test_context_init(&ctx);

    ASTModule *unit;
    const char *source = "module test;\nstruct Broken { value: Nope }";

    Resolver resolver = test_resolver(&ctx, NULL);

    Scope *module_scope = scope_create_kind(ctx.arena, ctx.global, SCOPE_MODULE);

    ResolvedModule *resolved;

    parse_module((const char *const[]){source}, 1, NULL, ctx.arena, &ctx.strings, &unit, &ctx.diagnostics);
    resolve_module(&resolver, unit, module_scope, (ModulePrivileges){0}, &resolved);

    assert(diagnostics_count(&ctx.diagnostics) == 1);

    test_context_free(&ctx);
}

static void test_field_lookup_misses() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Vec3 { x: f32, y: f32, z: f32 }", "Vec3", &registry);

    assert(type_registry_find_field(registry, type, string_from_cstr(&ctx.strings, "w")) == NULL);

    test_context_free(&ctx);
}

static void test_builtin_widths() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = ctx.types;

    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);
    const Type *f32_type = type_registry_get_primitive(registry, TYPE_F32);
    const Type *bool_type = type_registry_get_primitive(registry, TYPE_BOOL);

    assert(type_registry_size_of(registry, i32_type) == sizeof(int32_t));
    assert(type_registry_align_of(registry, i32_type) == _Alignof(int32_t));
    assert(type_registry_size_of(registry, f32_type) == sizeof(float));
    assert(type_registry_align_of(registry, f32_type) == _Alignof(float));
    assert(type_registry_size_of(registry, bool_type) == sizeof(_Bool));
    assert(type_registry_align_of(registry, bool_type) == _Alignof(_Bool));

    test_context_free(&ctx);
}

static void test_raw_pointer_owns_nothing() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = ctx.types;

    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    const Type *ptr = type_registry_raw_of(registry, i32_type);

    assert(type_kind(ptr) == TYPE_RAW);
    assert(type_pointee(ptr) == i32_type);

    assert(type_registry_raw_of(registry, i32_type) == ptr);

    assert(!type_registry_owns(registry, ptr));
    assert(type_registry_copies(registry, ptr));

    assert(ptr != type_registry_box_to(registry, i32_type));
    assert(ptr != type_registry_ref_to(registry, i32_type));

    assert(type_registry_size_of(registry, ptr) == sizeof(void *));
    assert(type_registry_align_of(registry, ptr) == _Alignof(void *));

    test_context_free(&ctx);
}

/* A type is interned under the arguments it was given, so two lengths name two array types and one
 * length names one, however many times it is asked for. */
static void test_an_array_is_interned_under_its_length() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = ctx.types;

    const Type *element = type_registry_get_primitive(registry, TYPE_I32);

    const Type *three = type_registry_array_of(registry, element, 3);
    const Type *four = type_registry_array_of(registry, element, 4);
    const Type *three_again = type_registry_array_of(registry, element, 3);

    assert(three != four);
    assert(three == three_again);

    assert(type_array_length(three) == 3);
    assert(type_array_length(four) == 4);

    test_context_free(&ctx);
}

static void test_a_borrow_and_a_box_are_distinct_constructors() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = ctx.types;

    const Type *i32_type = type_registry_get_primitive(registry, TYPE_I32);

    const Type *box = type_registry_box_to(registry, i32_type);
    const Type *ref = type_registry_ref_to(registry, i32_type);

    assert(type_kind(box) == TYPE_BOX);
    assert(type_kind(ref) == TYPE_REF);

    assert(type_pointee(box) == i32_type);
    assert(type_pointee(ref) == i32_type);

    assert(type_registry_box_to(registry, i32_type) == box);
    assert(type_registry_ref_to(registry, i32_type) == ref);

    assert(box != ref);
    assert(box != i32_type && ref != i32_type);

    assert(type_registry_owns(registry, box));
    assert(!type_registry_owns(registry, ref));

    assert(!type_registry_copies(registry, box));
    assert(type_registry_copies(registry, ref));

    test_context_free(&ctx);
}

static void test_mutually_recursive_structs() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *a = resolve_struct(&ctx,
                                   "struct A { b: *B }\n"
                                   "struct B { a: *A }\n",
                                   "A", &registry);

    const TypeField *field = type_registry_find_field(registry, a, string_from_cstr(&ctx.strings, "b"));

    assert(field);
    assert(type_name_of(type_pointee(field->type)) == string_from_cstr(&ctx.strings, "B"));

    test_context_free(&ctx);
}

static void test_a_containment_cycle_is_reported_once() {
    TestContext ctx;
    test_context_init(&ctx);

    ASTModule *unit;
    const char *source = test_in_a_module("struct A { b: B }\n"
                                          "struct B { a: A }\n");

    Resolver resolver = test_resolver(&ctx, NULL);

    Scope *module_scope = scope_create_kind(ctx.arena, ctx.global, SCOPE_MODULE);

    ResolvedModule *resolved;

    parse_module((const char *const[]){source}, 1, NULL, ctx.arena, &ctx.strings, &unit, &ctx.diagnostics);
    resolve_module(&resolver, unit, module_scope, (ModulePrivileges){0}, &resolved);

    assert(diagnostics_count(&ctx.diagnostics) == 1);

    test_context_free(&ctx);
}

static void test_array_of_a_struct_declared_below() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *holder = resolve_struct(&ctx,
                                        "struct Holder { cells: array<Cell, 2> }\n"
                                        "struct Cell { value: i32 }\n",
                                        "Holder", &registry);

    assert(type_registry_size_of(registry, holder) == 2 * sizeof(int32_t));

    test_context_free(&ctx);
}

static void test_a_ring_through_a_box_is_laid_out() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *a = resolve_struct(&ctx,
                                   "struct A { b: *B, tag: i32 }\n"
                                   "struct B { a: *A }\n",
                                   "A", &registry);

    assert(type_registry_size_of(registry, a) == sizeof(BoxRingC));
    assert(type_registry_align_of(registry, a) == _Alignof(BoxRingC));
    assert(offset_of(&ctx, registry, a, "tag") == offsetof(BoxRingC, tag));

    test_context_free(&ctx);
}

static void test_rejects_an_array_of_the_struct_declaring_it() {
    TestContext ctx;
    test_context_init(&ctx);

    ASTModule *unit;
    const char *source = test_in_a_module("struct A { cells: array<A, 2> }");

    Resolver resolver = test_resolver(&ctx, NULL);

    Scope *module_scope = scope_create_kind(ctx.arena, ctx.global, SCOPE_MODULE);

    ResolvedModule *resolved;

    parse_module((const char *const[]){source}, 1, NULL, ctx.arena, &ctx.strings, &unit, &ctx.diagnostics);
    resolve_module(&resolver, unit, module_scope, (ModulePrivileges){0}, &resolved);

    assert(diagnostics_count(&ctx.diagnostics) == 1);
    assert(strcmp(diagnostics_get(&ctx.diagnostics, 0)->message,
                  "struct 'A' cannot contain itself: 'A' contains 'A'") == 0);

    test_context_free(&ctx);
}

int main(void) {
    test_builtin_widths();
    test_raw_pointer_owns_nothing();
    test_a_borrow_and_a_box_are_distinct_constructors();
    test_an_array_is_interned_under_its_length();

    test_homogeneous_struct();
    test_interior_padding();
    test_trailing_padding();
    test_nested_struct();
    test_single_field_struct();
    test_empty_struct();
    test_trailing_comma_allowed();
    test_mutually_recursive_structs();
    test_a_containment_cycle_is_reported_once();
    test_array_of_a_struct_declared_below();
    test_a_ring_through_a_box_is_laid_out();
    test_rejects_an_array_of_the_struct_declaring_it();

    test_an_unknown_field_type_is_reported_once();
    test_field_lookup_misses();

    printf("All type layout tests passed\n");
    return 0;
}
