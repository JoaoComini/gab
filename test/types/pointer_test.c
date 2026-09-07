#include "ast/ast.h"
#include "scope.h"
#include "string/string.h"
#include "support/run.h"
#include "support/test_context.h"
#include "syntax/parser.h"
#include "type/type.h"
#include "type/type_registry.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static Binding *lookup(TestContext *ctx, Scope *scope, const char *name) {
    return scope_binding_lookup(scope, string_from_cstr(&ctx->strings, name));
}

static const Type *field_type(TestContext *ctx, Scope *scope, const char *struct_name, const char *field) {
    const Type *type = scope_type_lookup(scope, string_from_cstr(&ctx->strings, struct_name));

    return type_registry_find_field(scope->type_registry, type, string_from_cstr(&ctx->strings, field))->type;
}

static void test_pointer_types_are_interned() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "struct Player { health: i32 }\n"
                           "struct Holder { p: *Player, q: *Player }\n");
    assert(ok);

    const Type *p = field_type(&ctx, scope, "Holder", "p");
    const Type *q = field_type(&ctx, scope, "Holder", "q");

    assert(p && q);
    assert(p == q);
    assert(type_is_indirect(p));

    const Type *player = scope_type_lookup(scope, string_from_cstr(&ctx.strings, "Player"));
    assert(type_pointee(p) == player);

    test_context_free(&ctx);
}

static void test_pointer_depth_nests() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit, "struct Holder { p: *i32, q: **i32 }\n");
    assert(ok);

    const Type *p = field_type(&ctx, scope, "Holder", "p");
    const Type *q = field_type(&ctx, scope, "Holder", "q");

    assert(type_pointee(q) == p);
    assert(type_pointee(p) == scope_type_lookup(scope, string_from_cstr(&ctx.strings, "i32")));

    test_context_free(&ctx);
}

static void test_pointer_is_a_word() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "struct Big { a: i32, b: i32, c: i32, d: i32 }\n"
                           "struct Holder { p: *Big, q: *bool }\n");
    assert(ok);

    const Type *p = field_type(&ctx, scope, "Holder", "p");
    const Type *q = field_type(&ctx, scope, "Holder", "q");

    TypeRegistry *registry = scope->type_registry;

    assert(type_registry_size_of(registry, p) == sizeof(void *));
    assert(type_registry_align_of(registry, p) == _Alignof(void *));
    assert(type_registry_size_of(registry, q) == type_registry_size_of(registry, p));
    assert(type_registry_align_of(registry, q) == type_registry_align_of(registry, p));

    test_context_free(&ctx);
}

static void test_ref_is_a_distinct_type() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "struct Node { n: i32 }\n"
                           "struct Holder { o: *Node, b: &Node }\n");
    assert(ok);

    const Type *owning = field_type(&ctx, scope, "Holder", "o");
    const Type *borrow = field_type(&ctx, scope, "Holder", "b");

    assert(owning && borrow);
    assert(owning != borrow);
    assert(type_kind(owning) == TYPE_BOX);
    assert(type_kind(borrow) == TYPE_REF);

    assert(type_pointee(owning) == type_pointee(borrow));
    assert(type_registry_size_of(scope->type_registry, borrow) == sizeof(void *));

    test_context_free(&ctx);
}

static void test_ref_pointers_are_interned() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "struct Node { n: i32 }\n"
                           "let a: &Node;\n"
                           "let b: &Node;\n");
    assert(ok);

    Binding *a = scope_binding_lookup(scope, string_from_cstr(&ctx.strings, "a"));
    Binding *b = scope_binding_lookup(scope, string_from_cstr(&ctx.strings, "b"));

    assert(a->var.type == b->var.type);

    test_context_free(&ctx);
}

static void test_a_pointer_type_is_spelled_with_a_sigil() {
    assert(test_compiles("func f(): i32 { let p: *i32; return 0; }\n"));
    assert(!test_compiles("func f(): i32 { let p: box int; return 0; }\n"));
}

static void test_a_borrow_has_no_operator() {
    assert(test_compiles("func f(): i32 { let x: i32 = 1; let p: &i32 = x; return *p; }\n"));
    assert(!test_compiles("func f(): i32 { let x: i32 = 1; let p: &i32 = &x; return *p; }\n"));
}

int main() {
    test_a_pointer_type_is_spelled_with_a_sigil();
    test_a_borrow_has_no_operator();
    test_pointer_types_are_interned();
    test_pointer_depth_nests();
    test_pointer_is_a_word();
    test_ref_is_a_distinct_type();
    test_ref_pointers_are_interned();

    printf("pointer_test: all tests passed\n");
    return 0;
}
