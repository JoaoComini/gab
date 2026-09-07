#include "ast/ast.h"
#include "scope.h"
#include "string/string.h"
#include "support/run.h"
#include "support/test_context.h"
#include "syntax/parser.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/vm.h"

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

static void test_scalar_read_and_write_through_a_pointer() {
    assert(test_run_int("func f(): i32 { let x: i32 = 7; let p: &i32 = x; return *p; }\n"
                        "let r: i32 = f();") == 7);

    assert(test_run_int("func f(): i32 { let x: i32 = 3; let p: &i32 = x; *p = 42; return x; }\n"
                        "let r: i32 = f();") == 42);

    assert(test_run_float("func f(): f32 { let x: f32 = 1.5; let p: &f32 = x; *p = 2.5; return x; }\n"
                          "let r: f32 = f();") == 2.5f);
}

static void test_field_write_through_a_pointer() {
    assert(test_run_int("struct Player { health: i32, mana: i32 }\n"
                        "func f(): i32 { let p = Player { health: 1, mana: 2 };\n"
                        "let q: &Player = p; (*q).health = 10;\n"
                        "return p.health * 100 + p.mana; }\n"
                        "let r: i32 = f();") == 1002);
}

static void test_pointer_to_a_struct_field() {
    assert(test_run_int("struct Point { x: i32, y: i32 }\n"
                        "func f(): i32 { let v = Point { x: 1, y: 2 };\n"
                        "let p: &i32 = v.y; *p = 9;\n"
                        "return v.x * 100 + v.y; }\n"
                        "let r: i32 = f();") == 109);
}

static void test_pointer_to_a_sub_word_field() {
    assert(test_run_int("struct Flags { a: bool, b: bool, c: bool, d: bool }\n"
                        "func f(): i32 { let v = Flags { a: true, b: true, c: true, d: true };\n"
                        "let p: &bool = v.b; *p = false;\n"
                        "let n: i32 = 0;\n"
                        "if v.a { n = n + 1000; }\n"
                        "if v.b { n = n + 100; }\n"
                        "if v.c { n = n + 10; }\n"
                        "if v.d { n = n + 1; }\n"
                        "return n; }\n"
                        "let r: i32 = f();") == 1011);
}

static void test_dereferencing_a_whole_struct() {
    assert(test_run_int("struct Point { x: i32, y: i32 }\n"
                        "func f(): i32 { let v = Point { x: 3, y: 4 };\n"
                        "let p: &Point = v;\n"
                        "let copy: Point = *p;\n"
                        "v.x = 100;\n"
                        "return copy.x * 10 + copy.y; }\n"
                        "let r: i32 = f();") == 34);
}

static void test_pointer_to_a_pointer() {
    assert(test_run_int("func f(): i32 { let x: i32 = 5;\n"
                        "let p: &i32 = x;\n"
                        "let q: &&i32 = p;\n"
                        "**q = 11;\n"
                        "return x; }\n"
                        "let r: i32 = f();") == 11);
}

static void test_a_pointer_survives_a_stack_growth() {
    assert(test_run_int("func deep(n: i32, p: &i32): i32 {\n"
                        "if n > 0 { return deep(n - 1, p); }\n"
                        "return *p;\n"
                        "}\n"
                        "func f(): i32 { let x: i32 = 77; return deep(200, x); }\n"
                        "let r: i32 = f();") == 77);

    assert(test_run_int("func deep(n: i32, p: &i32): i32 {\n"
                        "if n > 0 { return deep(n - 1, p); }\n"
                        "*p = 88;\n"
                        "return 0;\n"
                        "}\n"
                        "func f(): i32 { let x: i32 = 1; let ignored: i32 = deep(200, x); return x; }\n"
                        "let r: i32 = f();") == 88);
}

static void test_field_access_auto_derefs() {
    assert(test_run_int("struct Player { health: i32, mana: i32 }\n"
                        "func f(): i32 { let p = Player { health: 1, mana: 2 };\n"
                        "let q: &Player = p; q.health = 10;\n"
                        "return p.health * 100 + p.mana; }\n"
                        "let r: i32 = f();") == 1002);

    assert(test_run_int("struct Player { health: i32, mana: i32 }\n"
                        "func f(): i32 { let p = Player { health: 10, mana: 0 };\n"
                        "let q: &Player = p; return q.health; }\n"
                        "let r: i32 = f();") == 10);
}

static void test_auto_deref_reaches_a_nested_field() {
    assert(test_run_int("struct Inner { v: i32 }\n"
                        "struct Outer { a: i32, inner: Inner }\n"
                        "func f(): i32 { let o = Outer { a: 0, inner: Inner { v: 0 } };\n"
                        "let q: &Outer = o;\n"
                        "q.inner.v = 7; return o.inner.v; }\n"
                        "let r: i32 = f();") == 7);
}

static void test_address_of_a_field_through_a_pointer() {
    assert(test_run_int("struct Player { health: i32, mana: i32 }\n"
                        "func f(): i32 { let p = Player { health: 0, mana: 2 };\n"
                        "let q: &Player = p;\n"
                        "let h: &i32 = q.health; *h = 9;\n"
                        "return p.health * 100 + p.mana; }\n"
                        "let r: i32 = f();") == 902);
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
    test_scalar_read_and_write_through_a_pointer();
    test_field_write_through_a_pointer();
    test_pointer_to_a_struct_field();
    test_pointer_to_a_sub_word_field();
    test_dereferencing_a_whole_struct();
    test_pointer_to_a_pointer();
    test_a_pointer_survives_a_stack_growth();
    test_field_access_auto_derefs();
    test_auto_deref_reaches_a_nested_field();
    test_address_of_a_field_through_a_pointer();

    printf("pointer_test: all tests passed\n");
    return 0;
}
