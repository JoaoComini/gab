#include "ast/resolve.h"
#include "lexer.h"
#include "parser.h"
#include "support/test_context.h"
#include "type.h"
#include "type_registry.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Equivalents the C compiler lays out, so every expectation below is checked
// against sizeof/offsetof rather than hand-computed numbers.
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
    Lexer lexer = lexer_create(test_in_a_module(source), ctx->arena, &ctx->strings, &ctx->diagnostics);
    Parser parser = parser_create(&lexer, &ctx->diagnostics);
    ASTUnit *unit = ast_unit_create();

    Scope global_scope;
    scope_init(&global_scope, ctx->arena, &ctx->strings, NULL);

    if (out_registry) {
        *out_registry = global_scope.type_registry;
    }

    if (parser_parse(&parser, unit)) {
        resolve_unit(ctx->arena, unit, &global_scope, NULL, &ctx->diagnostics);
    }

    if (diagnostics_has_errors(&ctx->diagnostics)) {
        diagnostics_print(&ctx->diagnostics, stderr);
    }

    assert(!diagnostics_has_errors(&ctx->diagnostics));

    ast_unit_destroy(unit);

    return scope_type_lookup(&global_scope, string_from_cstr(&ctx->strings, name));
}

static size_t offset_of(TestContext *ctx, TypeRegistry *registry, const Type *type, const char *field) {
    const TypeField *found = type_find_field(type, string_from_cstr(&ctx->strings, field));

    assert(found);

    return type_registry_layout_of(registry, type)->offsets[found - type_fields(type)];
}

static void test_homogeneous_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type =
        resolve_struct(&ctx, "struct Vec3 { x: float, y: float, z: float }", "Vec3", &registry);

    assert(type != NULL);
    assert(type->kind == TYPE_STRUCT);
    assert(type_field_count(type) == 3);

    assert(type_registry_size_of(registry, type) == sizeof(Vec3C));
    assert(type_registry_align_of(registry, type) == _Alignof(Vec3C));

    assert(offset_of(&ctx, registry, type, "x") == offsetof(Vec3C, x));
    assert(offset_of(&ctx, registry, type, "y") == offsetof(Vec3C, y));
    assert(offset_of(&ctx, registry, type, "z") == offsetof(Vec3C, z));

    test_context_free(&ctx);
}

// A bool followed by an int needs three bytes of padding; a naive layout that
// just sums sizes gets this wrong.
static void test_interior_padding() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Pad { flag: bool, value: int }", "Pad", &registry);

    assert(type_registry_size_of(registry, type) == sizeof(LeadingPadC));
    assert(type_registry_align_of(registry, type) == _Alignof(LeadingPadC));

    assert(offset_of(&ctx, registry, type, "flag") == offsetof(LeadingPadC, flag));
    assert(offset_of(&ctx, registry, type, "value") == offsetof(LeadingPadC, value));

    test_context_free(&ctx);
}

// The struct is rounded up to its own alignment, so this is 8 rather than 5.
static void test_trailing_padding() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Pad { value: int, flag: bool }", "Pad", &registry);

    assert(type_registry_size_of(registry, type) == sizeof(TrailingPadC));
    assert(offset_of(&ctx, registry, type, "value") == offsetof(TrailingPadC, value));
    assert(offset_of(&ctx, registry, type, "flag") == offsetof(TrailingPadC, flag));

    test_context_free(&ctx);
}

// Alignment propagates from the nested type itself, not from its fields.
static void test_nested_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx,
                                      "struct Vec3 { x: float, y: float, z: float }"
                                      "struct Nested { flag: bool, position: Vec3 }",
                                      "Nested", &registry);

    assert(type_registry_size_of(registry, type) == sizeof(NestedC));
    assert(type_registry_align_of(registry, type) == _Alignof(NestedC));

    assert(offset_of(&ctx, registry, type, "flag") == offsetof(NestedC, flag));
    assert(offset_of(&ctx, registry, type, "position") == offsetof(NestedC, position));

    const TypeField *position = type_find_field(type, string_from_cstr(&ctx.strings, "position"));
    assert(position->type->kind == TYPE_STRUCT);
    assert(type_registry_size_of(registry, position->type) == sizeof(Vec3C));

    test_context_free(&ctx);
}

static void test_single_field_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type = resolve_struct(&ctx, "struct Single { only: int }", "Single", &registry);

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
    assert(type_field_count(type) == 0);
    assert(type_registry_size_of(registry, type) == 0);
    assert(type_registry_align_of(registry, type) == 1);

    test_context_free(&ctx);
}

static void test_trailing_comma_allowed() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type =
        resolve_struct(&ctx, "struct Vec3 { x: float, y: float, z: float, }", "Vec3", &registry);

    assert(type_field_count(type) == 3);
    assert(type_registry_size_of(registry, type) == sizeof(Vec3C));

    test_context_free(&ctx);
}

static void test_unknown_field_type_is_not_registered() {
    TestContext ctx;
    test_context_init(&ctx);

    Lexer lexer = lexer_create("module test;\nstruct Broken { value: Nope }", ctx.arena, &ctx.strings,
                               &ctx.diagnostics);
    Parser parser = parser_create(&lexer, &ctx.diagnostics);
    ASTUnit *unit = ast_unit_create();

    Scope global_scope;
    scope_init(&global_scope, ctx.arena, &ctx.strings, NULL);

    parser_parse(&parser, unit);
    resolve_unit(ctx.arena, unit, &global_scope, NULL, &ctx.diagnostics);

    assert(diagnostics_count(&ctx.diagnostics) == 1);

    // A struct that failed to resolve must not become usable as a type.
    assert(scope_type_lookup(&global_scope, string_from_cstr(&ctx.strings, "Broken")) == NULL);

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

static void test_field_lookup_misses() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *type =
        resolve_struct(&ctx, "struct Vec3 { x: float, y: float, z: float }", "Vec3", &registry);

    assert(type_find_field(type, string_from_cstr(&ctx.strings, "w")) == NULL);

    test_context_free(&ctx);
}

static void test_builtin_widths() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope global_scope;
    scope_init(&global_scope, ctx.arena, &ctx.strings, NULL);
    TypeRegistry *registry = global_scope.type_registry;

    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);
    const Type *float_type = type_registry_get_builtin(registry, TYPE_FLOAT);
    const Type *bool_type = type_registry_get_builtin(registry, TYPE_BOOL);

    assert(type_registry_size_of(registry, int_type) == sizeof(int32_t));
    assert(type_registry_align_of(registry, int_type) == _Alignof(int32_t));
    assert(type_registry_size_of(registry, float_type) == sizeof(float));
    assert(type_registry_align_of(registry, float_type) == _Alignof(float));
    assert(type_registry_size_of(registry, bool_type) == sizeof(_Bool));
    assert(type_registry_align_of(registry, bool_type) == _Alignof(_Bool));

    test_context_free(&ctx);
}

// A 'ptr T' names an address and nothing more: it is interned on its pointee
// like every other constructed type, and it neither owns what it names nor
// stops a value holding one from copying. That is what separates it from both
// spellings of an indirection -- a 'box T' owns, and a 'ref T' borrows, while
// this makes no claim at all.
static void test_raw_pointer_owns_nothing() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope global_scope;
    scope_init(&global_scope, ctx.arena, &ctx.strings, NULL);
    TypeRegistry *registry = global_scope.type_registry;

    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const Type *ptr = type_registry_ptr_to(registry, int_type);

    assert(ptr->kind == TYPE_PTR);
    assert(type_pointee(ptr) == int_type);

    assert(type_registry_ptr_to(registry, int_type) == ptr);

    assert(!type_is_owned(ptr));
    assert(type_is_copyable(ptr));
    assert(type_registry_drop_of(registry, ptr) == NULL);

    // A 'box int' owns and a 'ref int' borrows; neither is this type.
    assert(ptr != type_registry_box_to(registry, int_type));
    assert(ptr != type_registry_ref_to(registry, int_type));

    assert(type_registry_size_of(registry, ptr) == sizeof(void *));
    assert(type_registry_align_of(registry, ptr) == _Alignof(void *));

    test_context_free(&ctx);
}

// A borrow and an ownership are separate constructors, each carrying what it
// names. Neither is the pointee, and neither is the other: what a slot must
// free is read off the kind rather than off a flag beside it.
static void test_a_borrow_and_a_box_are_distinct_constructors() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope global_scope;
    scope_init(&global_scope, ctx.arena, &ctx.strings, NULL);
    TypeRegistry *registry = global_scope.type_registry;

    const Type *int_type = type_registry_get_builtin(registry, TYPE_INT);

    const Type *box = type_registry_box_to(registry, int_type);
    const Type *ref = type_registry_ref_to(registry, int_type);

    assert(box->kind == TYPE_BOX);
    assert(ref->kind == TYPE_REF);

    assert(type_pointee(box) == int_type);
    assert(type_pointee(ref) == int_type);

    // Interned on the pointee, so a second mention is the same Type.
    assert(type_registry_box_to(registry, int_type) == box);
    assert(type_registry_ref_to(registry, int_type) == ref);

    assert(box != ref);
    assert(box != int_type && ref != int_type);

    // The whole difference between them: one frees what it names, one does not.
    assert(type_is_owned(box));
    assert(!type_is_owned(ref));

    assert(!type_is_copyable(box));
    assert(type_is_copyable(ref));

    test_context_free(&ctx);
}

// A 'box T' needs T's declaration, not its layout, so two structs may each
// point at the other however they are ordered in the file.
static void test_mutually_recursive_structs() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *a = resolve_struct(&ctx,
                                   "struct A { b: box B }\n"
                                   "struct B { a: box A }\n",
                                   "A", &registry);

    const TypeField *field = type_find_field(a, string_from_cstr(&ctx.strings, "b"));

    assert(field);
    assert(type_pointee(field->type)->name == string_from_cstr(&ctx.strings, "B"));

    test_context_free(&ctx);
}

// A struct whose field failed has no layout, and neither has anything holding
// it: a width derived from a type that has none would be wrong rather than
// missing.
static void test_a_failed_field_poisons_what_holds_it() {
    TestContext ctx;
    test_context_init(&ctx);

    Lexer lexer = lexer_create(test_in_a_module("struct A { b: B }\n"
                                                "struct B { a: A }\n"),
                               ctx.arena, &ctx.strings, &ctx.diagnostics);
    Parser parser = parser_create(&lexer, &ctx.diagnostics);
    ASTUnit *unit = ast_unit_create();

    Scope global_scope;
    scope_init(&global_scope, ctx.arena, &ctx.strings, NULL);

    parser_parse(&parser, unit);
    resolve_unit(ctx.arena, unit, &global_scope, NULL, &ctx.diagnostics);

    assert(scope_type_lookup(&global_scope, string_from_cstr(&ctx.strings, "A")) == NULL);
    assert(scope_type_lookup(&global_scope, string_from_cstr(&ctx.strings, "B")) == NULL);

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// An array's element is held by value, so naming one demands its layout -- and
// gets it, however far down the file the element was declared.
static void test_array_of_a_struct_declared_below() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *holder = resolve_struct(&ctx,
                                        "struct Holder { cells: Array Cell,2 }\n"
                                        "struct Cell { value: int }\n",
                                        "Holder", &registry);

    assert(type_registry_size_of(registry, holder) == 2 * sizeof(int32_t));

    test_context_free(&ctx);
}

// A ring through a 'box' is finite: the indirection is a machine word whatever
// it names, so neither struct's width waits on the other's.
static void test_a_ring_through_a_box_is_laid_out() {
    TestContext ctx;
    test_context_init(&ctx);

    TypeRegistry *registry = NULL;
    const Type *a = resolve_struct(&ctx,
                                   "struct A { b: box B, tag: int }\n"
                                   "struct B { a: box A }\n",
                                   "A", &registry);

    assert(type_registry_size_of(registry, a) == sizeof(BoxRingC));
    assert(type_registry_align_of(registry, a) == _Alignof(BoxRingC));
    assert(offset_of(&ctx, registry, a, "tag") == offsetof(BoxRingC, tag));

    test_context_free(&ctx);
}

// An array is a run of its element, so a struct holding an array of itself is
// as infinite as one holding itself directly.
static void test_rejects_an_array_of_the_struct_declaring_it() {
    TestContext ctx;
    test_context_init(&ctx);

    Lexer lexer = lexer_create(test_in_a_module("struct A { cells: Array A,2 }"), ctx.arena, &ctx.strings,
                               &ctx.diagnostics);
    Parser parser = parser_create(&lexer, &ctx.diagnostics);
    ASTUnit *unit = ast_unit_create();

    Scope global_scope;
    scope_init(&global_scope, ctx.arena, &ctx.strings, NULL);

    parser_parse(&parser, unit);
    resolve_unit(ctx.arena, unit, &global_scope, NULL, &ctx.diagnostics);

    assert(diagnostics_count(&ctx.diagnostics) == 1);
    assert(strcmp(diagnostics_get(&ctx.diagnostics, 0)->message, "struct 'A' cannot contain itself") == 0);
    assert(scope_type_lookup(&global_scope, string_from_cstr(&ctx.strings, "A")) == NULL);

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

int main(void) {
    test_builtin_widths();
    test_raw_pointer_owns_nothing();
    test_a_borrow_and_a_box_are_distinct_constructors();

    test_homogeneous_struct();
    test_interior_padding();
    test_trailing_padding();
    test_nested_struct();
    test_single_field_struct();
    test_empty_struct();
    test_trailing_comma_allowed();
    test_mutually_recursive_structs();
    test_a_failed_field_poisons_what_holds_it();
    test_array_of_a_struct_declared_below();
    test_a_ring_through_a_box_is_laid_out();
    test_rejects_an_array_of_the_struct_declaring_it();

    test_unknown_field_type_is_not_registered();
    test_field_lookup_misses();

    printf("All type layout tests passed\n");
    return 0;
}
