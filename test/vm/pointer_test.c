#include "ast/ast.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/run.h"
#include "support/test_context.h"
#include "type.h"
#include "type_registry.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static Symbol *lookup(TestContext *ctx, Scope *scope, const char *name) {
    return scope_symbol_lookup(scope, string_from_cstr(&ctx->strings, name));
}

// The whole type system compares types by pointer identity, so two mentions of
// 'box Player' must yield the same Type *.
static void test_pointer_types_are_interned() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Player { health: int }\n"
                           "let p: box Player;\n"
                           "let q: box Player;\n");
    assert(ok);

    Symbol *p = lookup(&ctx, scope, "p");
    Symbol *q = lookup(&ctx, scope, "q");

    assert(p && q);
    assert(p->var.type == q->var.type);
    assert(type_is_pointer(p->var.type));

    Type *player = scope_type_lookup(scope, string_from_cstr(&ctx.strings, "Player"));
    assert(p->var.type->pointee == player);

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// '**T' is a pointer to the interned '*T', not a second flavour of pointer.
static void test_pointer_depth_nests() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit, "let p: box int;\nlet q: box box int;\n");
    assert(ok);

    Symbol *p = lookup(&ctx, scope, "p");
    Symbol *q = lookup(&ctx, scope, "q");

    assert(q->var.type->pointee == p->var.type);
    assert(p->var.type->pointee == scope_type_lookup(scope, string_from_cstr(&ctx.strings, "int")));

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// A pointer is a raw address: 8 bytes wanting 8-byte alignment, whatever it
// points at.
static void test_pointer_is_a_word() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Big { a: int, b: int, c: int, d: int }\n"
                           "let p: box Big;\n"
                           "let q: box bool;\n");
    assert(ok);

    Symbol *p = lookup(&ctx, scope, "p");
    Symbol *q = lookup(&ctx, scope, "q");

    assert(p->var.type->size == sizeof(void *));
    assert(p->var.type->alignment == _Alignof(void *));
    assert(q->var.type->size == p->var.type->size);
    assert(q->var.type->alignment == p->var.type->alignment);

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// 'ref T' and '*T' are different types, so that freeing an object can tell from
// a field's type alone whether it owns what the field names.
static void test_ref_is_a_distinct_type() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Node { n: int }\n"
                           "let o: box Node;\n"
                           "let b: ref Node;\n");
    assert(ok);

    Symbol *owning = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "o"));
    Symbol *borrow = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "b"));

    assert(owning && borrow);
    assert(owning->var.type != borrow->var.type);
    assert(!owning->var.type->is_ref);
    assert(borrow->var.type->is_ref);

    // Same pointee, and both are still ordinary pointers: a borrow is the same
    // address, differing only in who frees the pointee.
    assert(owning->var.type->pointee == borrow->var.type->pointee);
    assert(borrow->var.type->size == sizeof(void *));

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// Interned like every other type, so two mentions of 'ref Node' are one Type.
static void test_ref_pointers_are_interned() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Node { n: int }\n"
                           "let a: ref Node;\n"
                           "let b: ref Node;\n");
    assert(ok);

    Symbol *a = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "a"));
    Symbol *b = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "b"));

    assert(a->var.type == b->var.type);

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// The address is a real address into the stack, so writing through it must be
// visible to the variable itself.
static void test_scalar_read_and_write_through_a_pointer() {
    assert(test_run_int("func f(): int { let x: int = 7; let p: ref int = ref x; return *p; }\n"
                        "let r: int = f();") == 7);

    assert(test_run_int("func f(): int { let x: int = 3; let p: ref int = ref x; *p = 42; return x; }\n"
                        "let r: int = f();") == 42);

    assert(test_run_float(
               "func f(): float { let x: float = 1.5; let p: ref float = ref x; *p = 2.5; return x; }\n"
               "let r: float = f();") == 2.5f);
}

// A field write through a pointer must land in the pointee and disturb nothing
// beside it.
static void test_field_write_through_a_pointer() {
    assert(test_run_int("struct Player { health: int, mana: int }\n"
                        "func f(): int { let p: Player; p.health = 1; p.mana = 2;\n"
                        "let q: ref Player = ref p; (*q).health = 10;\n"
                        "return p.health * 100 + p.mana; }\n"
                        "let r: int = f();") == 1002);
}

// A pointer to a field addresses that field alone, so writing through it must
// not touch its neighbours.
static void test_pointer_to_a_struct_field() {
    assert(test_run_int("struct Vec { x: int, y: int }\n"
                        "func f(): int { let v: Vec; v.x = 1; v.y = 2;\n"
                        "let p: ref int = ref v.y; *p = 9;\n"
                        "return v.x * 100 + v.y; }\n"
                        "let r: int = f();") == 109);
}

// Sub-word fields share a slot, so a pointer to one must still write only its
// own byte.
static void test_pointer_to_a_sub_word_field() {
    assert(test_run_int("struct Flags { a: bool, b: bool, c: bool, d: bool }\n"
                        "func f(): int { let v: Flags;\n"
                        "v.a = true; v.b = true; v.c = true; v.d = true;\n"
                        "let p: ref bool = ref v.b; *p = false;\n"
                        "let n: int = 0;\n"
                        "if v.a { n = n + 1000; }\n"
                        "if v.b { n = n + 100; }\n"
                        "if v.c { n = n + 10; }\n"
                        "if v.d { n = n + 1; }\n"
                        "return n; }\n"
                        "let r: int = f();") == 1011);
}

// Dereferencing a whole struct copies its slots out, so the copy is independent
// of the original.
static void test_dereferencing_a_whole_struct() {
    assert(test_run_int("struct Vec { x: int, y: int }\n"
                        "func f(): int { let v: Vec; v.x = 3; v.y = 4;\n"
                        "let p: ref Vec = ref v;\n"
                        "let copy: Vec = *p;\n"
                        "v.x = 100;\n"
                        "return copy.x * 10 + copy.y; }\n"
                        "let r: int = f();") == 34);
}

// A pointer to a pointer still resolves to one address at the end of the chain.
static void test_pointer_to_a_pointer() {
    assert(test_run_int("func f(): int { let x: int = 5;\n"
                        "let p: ref int = ref x;\n"
                        "let q: ref ref int = ref p;\n"
                        "**q = 11;\n"
                        "return x; }\n"
                        "let r: int = f();") == 11);
}

// Addresses point into a buffer that realloc may move. Recursing deep enough to
// force the growth while a pointer to an outer frame is live is what catches a
// missing rebase.
static void test_a_pointer_survives_a_stack_growth() {
    assert(test_run_int("func deep(n: int, p: ref int): int {\n"
                        "if n > 0 { return deep(n - 1, p); }\n"
                        "return *p;\n"
                        "}\n"
                        "func f(): int { let x: int = 77; return deep(200, ref x); }\n"
                        "let r: int = f();") == 77);

    // And writing through it, so the frame the address came from is what
    // actually changed.
    assert(test_run_int("func deep(n: int, p: ref int): int {\n"
                        "if n > 0 { return deep(n - 1, p); }\n"
                        "*p = 88;\n"
                        "return 0;\n"
                        "}\n"
                        "func f(): int { let x: int = 1; let ignored: int = deep(200, ref x); return x; }\n"
                        "let r: int = f();") == 88);
}

// 'p.health' where p is a '*Player' reaches through the pointer, the way Go
// and C's '->' do. A '*T' method receiver relies on this to read naturally.
static void test_field_access_auto_derefs() {
    assert(test_run_int("struct Player { health: int, mana: int }\n"
                        "func f(): int { let p: Player; p.health = 1; p.mana = 2;\n"
                        "let q: ref Player = ref p; q.health = 10;\n"
                        "return p.health * 100 + p.mana; }\n"
                        "let r: int = f();") == 1002);

    assert(test_run_int("struct Player { health: int, mana: int }\n"
                        "func f(): int { let p: Player; p.health = 10;\n"
                        "let q: ref Player = ref p; return q.health; }\n"
                        "let r: int = f();") == 10);
}

// Auto-deref composes with the inline layout of a nested struct: one address
// plus a summed offset still reaches the innermost field.
static void test_auto_deref_reaches_a_nested_field() {
    assert(test_run_int("struct Inner { v: int }\n"
                        "struct Outer { a: int, inner: Inner }\n"
                        "func f(): int { let o: Outer; let q: ref Outer = ref o;\n"
                        "q.inner.v = 7; return o.inner.v; }\n"
                        "let r: int = f();") == 7);
}

// Taking the address of a field reached through a pointer adds the offset to
// the address rather than to a slot index.
static void test_address_of_a_field_through_a_pointer() {
    assert(test_run_int("struct Player { health: int, mana: int }\n"
                        "func f(): int { let p: Player; p.mana = 2;\n"
                        "let q: ref Player = ref p;\n"
                        "let h: ref int = ref q.health; *h = 9;\n"
                        "return p.health * 100 + p.mana; }\n"
                        "let r: int = f();") == 902);
}

// A pointer type is spelled with a keyword, and the sigils that once spelled it
// mean only multiplication and bitwise-and now.
static void test_a_pointer_type_is_spelled_box() {
    assert(test_compiles("func f(): int { let p: box int; return 0; }\n"));
    assert(!test_compiles("func f(): int { let p: *int; return 0; }\n"));
}

// Address-of is spelled with a keyword, so '&' is no longer a prefix operator.
static void test_taking_an_address_is_spelled_ref() {
    assert(test_compiles("func f(): int { let x: int = 1; let p: ref int = ref x; return *p; }\n"));
    assert(!test_compiles("func f(): int { let x: int = 1; let p: ref int = &x; return *p; }\n"));
}

int main() {
    test_a_pointer_type_is_spelled_box();
    test_taking_an_address_is_spelled_ref();
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
