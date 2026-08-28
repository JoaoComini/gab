#include "ast/ast.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/run.h"
#include "support/test_context.h"
#include "type.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

// Compiles as far as resolution and hands back the scope, so a test can inspect
// the types and method tables the front end settled on.
static const Type *lookup_type(TestContext *ctx, Scope *scope, const char *name) {
    return scope_type_lookup(scope, string_from_cstr(&ctx->strings, name));
}

static Symbol *lookup_method(TestContext *ctx, Scope *scope, const char *type, const char *method) {
    return type_registry_find_method(scope->type_registry, lookup_type(ctx, scope, type),
                                     string_from_cstr(&ctx->strings, method));
}

// The receiver clause declares the function on the type rather than in a scope.
static void test_method_lands_on_its_receiver_type() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Player { health: int }\n"
                           "func Player::damage(p: ref Player, n: int): bool { return true; }\n");
    assert(ok);

    Symbol *damage = lookup_method(&ctx, scope, "Player", "damage");

    assert(damage);
    assert(damage->kind == SYMBOL_FUNC);

    // The receiver is parameter zero, so a one-parameter method has two.
    assert(damage->func.param_count == 2);
    assert(type_is_indirect(damage->func.params[0]));
    assert(type_pointee(damage->func.params[0]) == lookup_type(&ctx, scope, "Player"));

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// A method has no free-standing name: 'damage' alone must not resolve, or it
// would collide with a user's own declaration.
static void test_method_is_not_reachable_as_a_bare_name() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Player { health: int }\n"
                           "func Player::damage(p: ref Player, n: int): bool { return true; }\n");
    assert(ok);

    assert(!scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "damage")));

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// The whole reason methods are keyed by type: two structs may each declare an
// 'update', and the two are different functions.
static void test_same_name_on_two_types() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Player { health: int }\n"
                           "struct Enemy { health: int }\n"
                           "func Player::update(p: ref Player): int { return 1; }\n"
                           "func Enemy::update(e: ref Enemy): int { return 2; }\n");
    assert(ok);

    Symbol *player = lookup_method(&ctx, scope, "Player", "update");
    Symbol *enemy = lookup_method(&ctx, scope, "Enemy", "update");

    assert(player && enemy && player != enemy);

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// A value receiver is the struct itself, not a pointer to it.
static void test_value_receiver() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Player { health: int }\n"
                           "func Player::is_alive(p: Player): bool { return true; }\n");
    assert(ok);

    Symbol *alive = lookup_method(&ctx, scope, "Player", "is_alive");

    assert(alive);
    assert(alive->func.param_count == 1);
    assert(alive->func.params[0] == lookup_type(&ctx, scope, "Player"));

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// Declarations are hoisted, so a method may precede the struct it receives.
static void test_method_declared_above_its_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "func Player::health_of(p: ref Player): int { return p.health; }\n"
                           "struct Player { health: int }\n");
    assert(ok);

    assert(lookup_method(&ctx, scope, "Player", "health_of"));

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

// The receiver is an ordinary local in the body, so its fields resolve — and
// through a pointer receiver that means the existing auto-deref.
static void test_receiver_fields_resolve_in_the_body() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit,
                           "struct Player { health: int }\n"
                           "func Player::hp(p: ref Player): int { return p.health; }\n"
                           "func Player::hp2(q: Player): int { return q.health; }\n");
    assert(ok);

    ast_unit_destroy(unit);
    test_context_free(&ctx);
}

static bool fails(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit, source);

    ast_unit_destroy(unit);
    test_context_free(&ctx);

    return !ok;
}

static void test_diagnostics() {
    // One type may not declare the same method twice.
    assert(fails("struct Player { health: int }\n"
                 "func Player::update(p: ref Player): int { return 1; }\n"
                 "func Player::update(p: ref Player): int { return 2; }\n"));

    // A receiver has to be a struct; 'int' has no method set to hang one on.
    assert(fails("func int::double(n: int): int { return n; }\n"));

    // Nor a type that does not exist at all.
    assert(fails("func Missing::update(p: ref Missing): int { return 1; }\n"));
}

// A pointer receiver mutates what the caller holds, which is the whole reason
// to declare one.
static void test_pointer_receiver_mutates_the_caller() {
    assert(
        test_run_int(
            "struct Player { health: int }\n"
            "func Player::damage(p: ref Player, n: int): int { p.health = p.health - n; return p.health; }\n"
            "func main(): int {\n"
            "    let p: Player;\n"
            "    p.health = 100;\n"
            "    let ignored: int = p.damage(30);\n"
            "    return p.health;\n"
            "}\n"
            "let r: int = main();") == 70);
}

// A value receiver is a copy, so assigning to it leaves the caller's struct
// alone. The mirror of the test above, and the reason the two modes exist.
static void test_value_receiver_does_not_mutate_the_caller() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func Player::zero(p: Player): int { p.health = 0; return p.health; }\n"
                        "func main(): int {\n"
                        "    let p: Player;\n"
                        "    p.health = 55;\n"
                        "    let ignored: int = p.zero();\n"
                        "    return p.health;\n"
                        "}\n"
                        "let r: int = main();") == 55);
}

// Methods are keyed by receiver type, so two same-named methods are two
// different prototypes and the call must reach the right one.
static void test_same_name_dispatches_by_type() {
    assert(test_run_int("struct Player { health: int }\n"
                        "struct Enemy { health: int }\n"
                        "func Player::tag(p: ref Player): int { return 1; }\n"
                        "func Enemy::tag(e: ref Enemy): int { return 2; }\n"
                        "func main(): int {\n"
                        "    let p: Player;\n"
                        "    let e: Enemy;\n"
                        "    return p.tag() * 10 + e.tag();\n"
                        "}\n"
                        "let r: int = main();") == 12);
}

// A method calling another on the same receiver: the inner call reserves its
// own argument block inside the outer one's frame.
static void test_method_calls_another_method() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func Player::hp(p: ref Player): int { return p.health; }\n"
                        "func Player::double_hp(p: ref Player): int { return p.hp() + p.hp(); }\n"
                        "func main(): int {\n"
                        "    let p: Player;\n"
                        "    p.health = 21;\n"
                        "    return p.double_hp();\n"
                        "}\n"
                        "let r: int = main();") == 42);
}

// A method takes arguments after the receiver, and the arity the user writes
// excludes it.
static void test_method_arguments() {
    assert(test_run_int(
               "struct Vec { x: int, y: int }\n"
               "func Vec::set(v: ref Vec, a: int, b: int): int { v.x = a; v.y = b; return v.x + v.y; }\n"
               "func main(): int {\n"
               "    let v: Vec;\n"
               "    return v.set(3, 4);\n"
               "}\n"
               "let r: int = main();") == 7);
}

// Recursion through a method, so each invocation gets its own frame and its own
// copy of the receiver slot.
static void test_recursive_method() {
    assert(test_run_int("struct Counter { n: int }\n"
                        "func Counter::countdown(c: ref Counter, n: int): int {\n"
                        "    if n <= 0 { return c.n; }\n"
                        "    c.n = c.n + n;\n"
                        "    return c.countdown(n - 1);\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let c: Counter;\n"
                        "    c.n = 0;\n"
                        "    return c.countdown(4);\n"
                        "}\n"
                        "let r: int = main();") == 10);
}

// A method may be called on a receiver that is already a pointer, where no
// address needs taking.
static void test_call_through_a_pointer_receiver() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func Player::hp(p: ref Player): int { return p.health; }\n"
                        "func main(): int {\n"
                        "    let p: Player;\n"
                        "    p.health = 9;\n"
                        "    let q: ref Player = p;\n"
                        "    return q.hp();\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// A value method reached through a pointer copies the inner in.
static void test_value_method_through_a_pointer() {
    assert(test_run_int("struct Player { health: int }\n"
                        "func Player::hp(p: Player): int { return p.health; }\n"
                        "func main(): int {\n"
                        "    let p: Player;\n"
                        "    p.health = 13;\n"
                        "    let q: ref Player = p;\n"
                        "    return q.hp();\n"
                        "}\n"
                        "let r: int = main();") == 13);
}

// A struct parameter and a struct return share the argument block with the
// receiver, so their slots must not overlap it.
static void test_struct_parameter_and_return() {
    assert(test_run_int("struct Vec { x: int, y: int }\n"
                        "struct Adder { bias: int }\n"
                        "func Adder::add(a: ref Adder, v: Vec): Vec {\n"
                        "    let out: Vec;\n"
                        "    out.x = v.x + a.bias;\n"
                        "    out.y = v.y + a.bias;\n"
                        "    return out;\n"
                        "}\n"
                        "func main(): int {\n"
                        "    let a: Adder;\n"
                        "    a.bias = 10;\n"
                        "    let v: Vec;\n"
                        "    v.x = 1;\n"
                        "    v.y = 2;\n"
                        "    let out: Vec = a.add(v);\n"
                        "    return out.x * 100 + out.y;\n"
                        "}\n"
                        "let r: int = main();") == 1112);
}

// A method may be called on a struct held in a field of another struct, where
// the address taken is of the inner struct rather than a bare local.
static void test_call_on_a_nested_struct() {
    assert(test_run_int("struct Inner { n: int }\n"
                        "struct Outer { inner: Inner }\n"
                        "func Inner::bump(i: ref Inner): int { i.n = i.n + 1; return i.n; }\n"
                        "func main(): int {\n"
                        "    let o: Outer;\n"
                        "    o.inner.n = 5;\n"
                        "    let ignored: int = o.inner.bump();\n"
                        "    return o.inner.n;\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

static void test_call_diagnostics() {
    // An unknown method names the receiver's type, which is the useful half.
    assert(fails("struct Player { health: int }\n"
                 "func main(): int { let p: Player; return p.nope(); }\n"));

    // The arity a caller sees excludes the receiver.
    assert(fails("struct Player { health: int }\n"
                 "func Player::hp(p: ref Player): int { return p.health; }\n"
                 "func main(): int { let p: Player; return p.hp(1); }\n"));

    // Argument types are checked past the receiver.
    assert(fails("struct Player { health: int }\n"
                 "func Player::set(p: ref Player, n: int): int { return n; }\n"
                 "func main(): int { let p: Player; return p.set(true); }\n"));

    // A pointer receiver needs something with an address; a call result is a
    // temporary and has none.
    assert(fails("struct Player { health: int }\n"
                 "func Player::hp(p: ref Player): int { return p.health; }\n"
                 "func make(): Player { let p: Player; return p; }\n"
                 "func main(): int { return make().hp(); }\n"));

    // A scalar has no method set at all.
    assert(fails("func main(): int { let n: int = 1; return n.hp(); }\n"));
}

// Resolution rewrites 'recv.m(a)' into 'm(recv', a)', so the three ways a
// receiver reaches parameter zero all have to survive the rewrite. Each is one
// adjustment: none, an address taken, a inner copied in.

// The receiver already matches parameter zero, so it is passed as written.
static void test_a_matching_receiver_needs_no_adjustment() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func Box::get(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let b: box Box = new Box;\n"
                        "    b.n = 7;\n"
                        "    return b.get();\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

// A 'ref T' method called on a 'T' takes the receiver's address, which the rewrite
// spells as a real address-of node.
static void test_a_value_receiver_has_its_address_taken() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func Box::bump(b: ref Box): int { b.n = b.n + 1; return b.n; }\n"
                        "func main(): int {\n"
                        "    let b: Box;\n"
                        "    b.n = 4;\n"
                        "    let got: int = b.bump();\n"
                        "    return got * 10 + b.n;\n"
                        "}\n"
                        "let r: int = main();") == 55);
}

// A 'T' method called through a 'box T' copies the inner in, which the rewrite
// spells as a '*recv' node.
static void test_a_pointer_receiver_is_dereferenced() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func Box::peek(b: Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let b: box Box = new Box;\n"
                        "    b.n = 9;\n"
                        "    return b.peek();\n"
                        "}\n"
                        "let r: int = main();") == 9);
}

// A borrowed receiver reaches the same object its owner names, so a method
// called on one is ordinary and reads what the owner holds.
static void test_a_method_on_a_ref_receiver() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func Box::get(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: box Box = new Box;\n"
                        "    owner.n = 6;\n"
                        "    let borrowed: ref Box = owner;\n"
                        "    return borrowed.get();\n"
                        "}\n"
                        "let r: int = main();") == 6);
}

// Writing through a borrowed receiver reaches the same object too: a borrow is
// the same address, so the mutation is visible to the owner.
static void test_a_method_through_a_ref_mutates_the_owned_object() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func Box::bump(b: ref Box): int { b.n = b.n + 1; return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: box Box = new Box;\n"
                        "    owner.n = 4;\n"
                        "    let borrowed: ref Box = owner;\n"
                        "    borrowed.bump();\n"
                        "    return owner.n;\n"
                        "}\n"
                        "let r: int = main();") == 5);
}

// A method never owns its receiver, so declaring one 'box T' spells an ownership
// it cannot have -- and would let one method be written two ways that behave
// identically. 'ref T' is the one form, and 'T' by value the other.
// A receiver two levels out from what the method takes: 'ref box Box' reaching
// a 'ref Box' method, which is one hop to the 'box Box' and a lend from there.
static void test_a_receiver_two_levels_out_reaches_the_method() {
    assert(test_run_int("struct Box { n: int }\n"
                        "func Box::get(b: ref Box): int { return b.n; }\n"
                        "func main(): int {\n"
                        "    let owner: box Box = new Box;\n"
                        "    owner.n = 7;\n"
                        "    let outer: ref box Box = owner;\n"
                        "    return outer.get();\n"
                        "}\n"
                        "let r: int = main();") == 7);
}

// A function consuming parameter zero is declared like any other, and is
// reached where the transfer can be written rather than through a value.
static void test_a_consuming_function_is_not_reached_through_a_value() {
    assert(test_compiles("struct Box { n: int }\n"
                         "func Box::peek(b: box Box): int { return b.n; }\n"));

    assert(!test_compiles("struct Box { n: int }\n"
                          "func Box::peek(b: box Box): int { return b.n; }\n"
                          "func main(): int {\n"
                          "    let a: box Box = new Box;\n"
                          "    return a.peek();\n"
                          "}\n"));
}

// A method on a temporary that takes a pointer receiver has no address to take,
// and the rewrite must not invent one.
static void test_a_pointer_method_on_a_temporary_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func Box::get(b: ref Box): int { return b.n; }\n"
                          "func make(): Box { let b: Box; return b; }\n"
                          "func main(): int { return make().get(); }\n"));
}

// The receiver is argument zero after the rewrite, so an arity error must still
// count only the arguments the user actually wrote.
static void test_arity_errors_exclude_the_receiver() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func Box::add(b: ref Box, x: int): int { return b.n + x; }\n"
                          "func main(): int {\n"
                          "    let b: box Box = new Box;\n"
                          "    return b.add(1, 2);\n"
                          "}\n"));
}

// A name that is not a method is still a miss, and reports as one rather than
// as a missing field: the call target looks like a field until it resolves.
static void test_an_unknown_method_is_refused() {
    assert(!test_compiles("struct Box { n: int }\n"
                          "func main(): int {\n"
                          "    let b: box Box = new Box;\n"
                          "    return b.nope();\n"
                          "}\n"));
}

// Parameter zero by value takes ownership of what it is given, which the call
// site spells; borrowing it is 'ref T'. Both are ordinary parameter forms, so a
// type that owns may be written either way.
static void test_parameter_zero_takes_the_form_it_declares() {
    assert(test_compiles("struct Node { child: box Node }\n"
                         "func Node::peek(n: Node): int { return 0; }\n"));

    assert(test_compiles("struct Node { child: box Node }\n"
                         "func Node::peek(n: ref Node): int { return 0; }\n"));

    assert(test_compiles("struct Point { x: int, y: int }\n"
                         "func Point::peek(p: Point): int { return p.x; }\n"));

    // The sugar copies what it reaches, so an owning type is passed where the
    // transfer can be written rather than through a value.
    assert(!test_compiles("struct Node { child: box Node }\n"
                          "func Node::peek(n: Node): int { return 0; }\n"
                          "func main(): int { let a: Node; return a.peek(); }\n"));
}

// A method reached through a shared method set still has to be given the
// receiver it declared: taking the address of one type does not produce a
// pointer to another, however the lookup arrived at the method.
static void test_a_pointer_receiver_is_not_reached_from_another_type() {
    assert(!test_compiles_on_vm("func f(): int {\n"
                                "    let s: ref str = \"hi\";\n"
                                "    let c: String = s.clone();\n"
                                "    return 0;\n"
                                "}\n"));

    assert(test_compiles_on_vm("func f(): int {\n"
                               "    let o: String = \"hi\".to_owned();\n"
                               "    let c: String = o.clone();\n"
                               "    return 0;\n"
                               "}\n"));
}

int main(void) {
    test_a_matching_receiver_needs_no_adjustment();
    test_a_value_receiver_has_its_address_taken();
    test_a_pointer_receiver_is_dereferenced();
    test_a_method_on_a_ref_receiver();
    test_a_method_through_a_ref_mutates_the_owned_object();
    test_a_receiver_two_levels_out_reaches_the_method();
    test_a_consuming_function_is_not_reached_through_a_value();
    test_a_pointer_method_on_a_temporary_is_refused();
    test_arity_errors_exclude_the_receiver();
    test_an_unknown_method_is_refused();
    test_method_lands_on_its_receiver_type();
    test_method_is_not_reachable_as_a_bare_name();
    test_same_name_on_two_types();
    test_value_receiver();
    test_method_declared_above_its_struct();
    test_receiver_fields_resolve_in_the_body();
    test_diagnostics();
    test_pointer_receiver_mutates_the_caller();
    test_value_receiver_does_not_mutate_the_caller();
    test_same_name_dispatches_by_type();
    test_method_calls_another_method();
    test_method_arguments();
    test_recursive_method();
    test_call_through_a_pointer_receiver();
    test_value_method_through_a_pointer();
    test_struct_parameter_and_return();
    test_call_on_a_nested_struct();
    test_call_diagnostics();
    test_parameter_zero_takes_the_form_it_declares();
    test_a_pointer_receiver_is_not_reached_from_another_type();

    printf("All method tests passed\n");
    return 0;
}
