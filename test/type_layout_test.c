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

static Type *resolve_struct(TestContext *ctx, const char *source, const char *name) {
    Lexer lexer = lexer_create(test_in_a_module(source), ctx->arena, &ctx->strings, &ctx->diagnostics);
    Parser parser = parser_create(&lexer, &ctx->diagnostics);
    ASTScript *script = ast_script_create();

    Scope global_scope;
    scope_init(&global_scope, ctx->arena, &ctx->strings, NULL);

    if (parser_parse(&parser, script)) {
        ast_script_resolve(ctx->arena, script, &global_scope, NULL, &ctx->diagnostics);
    }

    if (diagnostics_has_errors(&ctx->diagnostics)) {
        diagnostics_print(&ctx->diagnostics, stderr);
    }

    assert(!diagnostics_has_errors(&ctx->diagnostics));

    ast_script_destroy(script);

    return scope_type_lookup(&global_scope, string_from_cstr(&ctx->strings, name));
}

static size_t offset_of(TestContext *ctx, Type *type, const char *field) {
    size_t offset = 0;
    bool found = type_field_offset(type, string_from_cstr(&ctx->strings, field), &offset);

    assert(found);

    return offset;
}

static void test_homogeneous_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    Type *type = resolve_struct(&ctx, "struct Vec3 { x: float, y: float, z: float }", "Vec3");

    assert(type != NULL);
    assert(type->kind == TYPE_STRUCT);
    assert(type->field_count == 3);

    assert(type->size == sizeof(Vec3C));
    assert(type->alignment == _Alignof(Vec3C));

    assert(offset_of(&ctx, type, "x") == offsetof(Vec3C, x));
    assert(offset_of(&ctx, type, "y") == offsetof(Vec3C, y));
    assert(offset_of(&ctx, type, "z") == offsetof(Vec3C, z));

    test_context_free(&ctx);
}

// A bool followed by an int needs three bytes of padding; a naive layout that
// just sums sizes gets this wrong.
static void test_interior_padding() {
    TestContext ctx;
    test_context_init(&ctx);

    Type *type = resolve_struct(&ctx, "struct Pad { flag: bool, value: int }", "Pad");

    assert(type->size == sizeof(LeadingPadC));
    assert(type->alignment == _Alignof(LeadingPadC));

    assert(offset_of(&ctx, type, "flag") == offsetof(LeadingPadC, flag));
    assert(offset_of(&ctx, type, "value") == offsetof(LeadingPadC, value));

    test_context_free(&ctx);
}

// The struct is rounded up to its own alignment, so this is 8 rather than 5.
static void test_trailing_padding() {
    TestContext ctx;
    test_context_init(&ctx);

    Type *type = resolve_struct(&ctx, "struct Pad { value: int, flag: bool }", "Pad");

    assert(type->size == sizeof(TrailingPadC));
    assert(offset_of(&ctx, type, "value") == offsetof(TrailingPadC, value));
    assert(offset_of(&ctx, type, "flag") == offsetof(TrailingPadC, flag));

    test_context_free(&ctx);
}

// Alignment propagates from the nested type itself, not from its fields.
static void test_nested_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    Type *type = resolve_struct(&ctx,
                                "struct Vec3 { x: float, y: float, z: float }"
                                "struct Nested { flag: bool, position: Vec3 }",
                                "Nested");

    assert(type->size == sizeof(NestedC));
    assert(type->alignment == _Alignof(NestedC));

    assert(offset_of(&ctx, type, "flag") == offsetof(NestedC, flag));
    assert(offset_of(&ctx, type, "position") == offsetof(NestedC, position));

    const TypeField *position = type_find_field(type, string_from_cstr(&ctx.strings, "position"));
    assert(position->type->kind == TYPE_STRUCT);
    assert(position->type->size == sizeof(Vec3C));

    test_context_free(&ctx);
}

static void test_single_field_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    Type *type = resolve_struct(&ctx, "struct Single { only: int }", "Single");

    assert(type->size == sizeof(SingleC));
    assert(type->alignment == _Alignof(SingleC));
    assert(offset_of(&ctx, type, "only") == offsetof(SingleC, only));

    test_context_free(&ctx);
}

static void test_empty_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    Type *type = resolve_struct(&ctx, "struct Empty { }", "Empty");

    assert(type != NULL);
    assert(type->field_count == 0);
    assert(type->size == 0);
    assert(type->alignment == 1);

    test_context_free(&ctx);
}

static void test_trailing_comma_allowed() {
    TestContext ctx;
    test_context_init(&ctx);

    Type *type = resolve_struct(&ctx, "struct Vec3 { x: float, y: float, z: float, }", "Vec3");

    assert(type->field_count == 3);
    assert(type->size == sizeof(Vec3C));

    test_context_free(&ctx);
}

static void test_unknown_field_type_is_not_registered() {
    TestContext ctx;
    test_context_init(&ctx);

    Lexer lexer = lexer_create("module test;\nstruct Broken { value: Nope }", ctx.arena, &ctx.strings,
                               &ctx.diagnostics);
    Parser parser = parser_create(&lexer, &ctx.diagnostics);
    ASTScript *script = ast_script_create();

    Scope global_scope;
    scope_init(&global_scope, ctx.arena, &ctx.strings, NULL);

    parser_parse(&parser, script);
    ast_script_resolve(ctx.arena, script, &global_scope, NULL, &ctx.diagnostics);

    assert(diagnostics_count(&ctx.diagnostics) == 1);

    // A struct that failed to resolve must not become usable as a type.
    assert(scope_type_lookup(&global_scope, string_from_cstr(&ctx.strings, "Broken")) == NULL);

    ast_script_destroy(script);
    test_context_free(&ctx);
}

static void test_field_lookup_misses() {
    TestContext ctx;
    test_context_init(&ctx);

    Type *type = resolve_struct(&ctx, "struct Vec3 { x: float, y: float, z: float }", "Vec3");

    size_t offset = 0;
    assert(!type_field_offset(type, string_from_cstr(&ctx.strings, "w"), &offset));
    assert(type_find_field(type, string_from_cstr(&ctx.strings, "w")) == NULL);

    test_context_free(&ctx);
}

static void test_builtin_widths() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope global_scope;
    scope_init(&global_scope, ctx.arena, &ctx.strings, NULL);
    TypeRegistry *registry = global_scope.type_registry;

    Type *int_type = type_registry_get_builtin(registry, TYPE_INT);
    Type *float_type = type_registry_get_builtin(registry, TYPE_FLOAT);
    Type *bool_type = type_registry_get_builtin(registry, TYPE_BOOL);

    assert(int_type->size == sizeof(int32_t));
    assert(int_type->alignment == _Alignof(int32_t));
    assert(float_type->size == sizeof(float));
    assert(float_type->alignment == _Alignof(float));
    assert(bool_type->size == sizeof(_Bool));
    assert(bool_type->alignment == _Alignof(_Bool));

    test_context_free(&ctx);
}

int main(void) {
    test_builtin_widths();

    test_homogeneous_struct();
    test_interior_padding();
    test_trailing_padding();
    test_nested_struct();
    test_single_field_struct();
    test_empty_struct();
    test_trailing_comma_allowed();

    test_unknown_field_type_is_not_registered();
    test_field_lookup_misses();

    printf("All type layout tests passed\n");
    return 0;
}
