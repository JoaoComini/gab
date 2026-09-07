#include "ast/ast.h"
#include "scope.h"
#include "string/string.h"
#include "support/run.h"
#include "support/test_context.h"
#include "syntax/parser.h"
#include "type/type.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static const Type *lookup_type(TestContext *ctx, Scope *scope, const char *name) {
    return scope_type_lookup(scope, string_from_cstr(&ctx->strings, name));
}

static Function *lookup_method(TestContext *ctx, Scope *scope, const char *type, const char *method) {
    return type_registry_find_owned(scope->type_registry, lookup_type(ctx, scope, type),
                                    string_from_cstr(&ctx->strings, method));
}

static void test_method_lands_on_its_receiver_type() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "struct Player { health: i32 }\n"
                           "impl Player {\n"
                           "    func damage(p: &Player, n: i32): bool { return true; }\n"
                           "}\n");
    assert(ok);

    Function *damage = lookup_method(&ctx, scope, "Player", "damage");

    assert(damage);

    assert(damage->param_count == 2);
    assert(type_is_indirect(damage->params[0]));
    assert(type_pointee(damage->params[0]) == lookup_type(&ctx, scope, "Player"));

    test_context_free(&ctx);
}

static void test_method_is_not_reachable_as_a_bare_name() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "struct Player { health: i32 }\n"
                           "impl Player {\n"
                           "    func damage(p: &Player, n: i32): bool { return true; }\n"
                           "}\n");
    assert(ok);

    assert(!scope_binding_lookup(scope, string_from_cstr(&ctx.strings, "damage")));

    test_context_free(&ctx);
}

static void test_same_name_on_two_types() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "struct Player { health: i32 }\n"
                           "struct Enemy { health: i32 }\n"
                           "impl Player {\n"
                           "    func update(p: &Player): i32 { return 1; }\n"
                           "}\n"
                           "impl Enemy {\n"
                           "    func update(e: &Enemy): i32 { return 2; }\n"
                           "}\n");
    assert(ok);

    Function *player = lookup_method(&ctx, scope, "Player", "update");
    Function *enemy = lookup_method(&ctx, scope, "Enemy", "update");

    assert(player && enemy && player != enemy);

    test_context_free(&ctx);
}

static void test_value_receiver() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "struct Player { health: i32 }\n"
                           "impl Player {\n"
                           "    func is_alive(p: Player): bool { return true; }\n"
                           "}\n");
    assert(ok);

    Function *alive = lookup_method(&ctx, scope, "Player", "is_alive");

    assert(alive);
    assert(alive->param_count == 1);
    assert(alive->params[0] == lookup_type(&ctx, scope, "Player"));

    test_context_free(&ctx);
}

static void test_method_declared_above_its_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "impl Player {\n"
                           "    func health_of(p: &Player): i32 { return p.health; }\n"
                           "}\n"
                           "struct Player { health: i32 }\n");
    assert(ok);

    assert(lookup_method(&ctx, scope, "Player", "health_of"));

    test_context_free(&ctx);
}

static void test_receiver_fields_resolve_in_the_body() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit,
                           "struct Player { health: i32 }\n"
                           "impl Player {\n"
                           "    func hp(p: &Player): i32 { return p.health; }\n"
                           "}\n"
                           "impl Player {\n"
                           "    func hp2(q: Player): i32 { return q.health; }\n"
                           "}\n");
    assert(ok);

    test_context_free(&ctx);
}

static bool fails(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit;

    bool ok = test_resolve(&ctx, scope, &unit, source);

    test_context_free(&ctx);

    return !ok;
}

static void test_diagnostics() {
    assert(fails("struct Player { health: i32 }\n"
                 "impl Player {\n"
                 "    func update(p: &Player): i32 { return 1; }\n"
                 "}\n"
                 "impl Player {\n"
                 "    func update(p: &Player): i32 { return 2; }\n"
                 "}\n"));
}

static void test_pointer_receiver_mutates_the_caller() {
    assert(test_run_int(
               "struct Player { health: i32 }\n"
               "impl Player {\n"
               "    func damage(p: &Player, n: i32): i32 { p.health = p.health - n; return p.health; }\n"
               "}\n"
               "func main(): i32 {\n"
               "    let p = Player { health: 100 };\n"
               "    let ignored: i32 = p.damage(30);\n"
               "    return p.health;\n"
               "}\n"
               "let r: i32 = main();") == 70);
}

static void test_value_receiver_does_not_mutate_the_caller() {
    assert(test_run_int("struct Player { health: i32 }\n"
                        "impl Player {\n"
                        "    func zero(p: Player): i32 { p.health = 0; return p.health; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let p = Player { health: 55 };\n"
                        "    let ignored: i32 = p.zero();\n"
                        "    return p.health;\n"
                        "}\n"
                        "let r: i32 = main();") == 55);
}

static void test_same_name_dispatches_by_type() {
    assert(test_run_int("struct Player { health: i32 }\n"
                        "struct Enemy { health: i32 }\n"
                        "impl Player {\n"
                        "    func tag(p: &Player): i32 { return 1; }\n"
                        "}\n"
                        "impl Enemy {\n"
                        "    func tag(e: &Enemy): i32 { return 2; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let p = Player { health: 0 };\n"
                        "    let e = Enemy { health: 0 };\n"
                        "    return p.tag() * 10 + e.tag();\n"
                        "}\n"
                        "let r: i32 = main();") == 12);
}

static void test_method_calls_another_method() {
    assert(test_run_int("struct Player { health: i32 }\n"
                        "impl Player {\n"
                        "    func hp(p: &Player): i32 { return p.health; }\n"
                        "}\n"
                        "impl Player {\n"
                        "    func double_hp(p: &Player): i32 { return p.hp() + p.hp(); }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let p = Player { health: 21 };\n"
                        "    return p.double_hp();\n"
                        "}\n"
                        "let r: i32 = main();") == 42);
}

static void test_method_arguments() {
    assert(
        test_run_int("struct Point { x: i32, y: i32 }\n"
                     "impl Point {\n"
                     "    func set(v: &Point, a: i32, b: i32): i32 { v.x = a; v.y = b; return v.x + v.y; }\n"
                     "}\n"
                     "func main(): i32 {\n"
                     "    let v = Point { x: 0, y: 0 };\n"
                     "    return v.set(3, 4);\n"
                     "}\n"
                     "let r: i32 = main();") == 7);
}

static void test_recursive_method() {
    assert(test_run_int("struct Counter { n: i32 }\n"
                        "impl Counter {\n"
                        "    func countdown(c: &Counter, n: i32): i32 {\n"
                        "        if n <= 0 { return c.n; }\n"
                        "        c.n = c.n + n;\n"
                        "        return c.countdown(n - 1);\n"
                        "    }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let c = Counter { n: 0 };\n"
                        "    return c.countdown(4);\n"
                        "}\n"
                        "let r: i32 = main();") == 10);
}

static void test_call_through_a_pointer_receiver() {
    assert(test_run_int("struct Player { health: i32 }\n"
                        "impl Player {\n"
                        "    func hp(p: &Player): i32 { return p.health; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let p = Player { health: 9 };\n"
                        "    let q: &Player = p;\n"
                        "    return q.hp();\n"
                        "}\n"
                        "let r: i32 = main();") == 9);
}

static void test_value_method_through_a_pointer() {
    assert(test_run_int("struct Player { health: i32 }\n"
                        "impl Player {\n"
                        "    func hp(p: Player): i32 { return p.health; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let p = Player { health: 13 };\n"
                        "    let q: &Player = p;\n"
                        "    return q.hp();\n"
                        "}\n"
                        "let r: i32 = main();") == 13);
}

static void test_struct_parameter_and_return() {
    assert(test_run_int("struct Point { x: i32, y: i32 }\n"
                        "struct Adder { bias: i32 }\n"
                        "impl Adder {\n"
                        "    func add(a: &Adder, v: Point): Point {\n"
                        "        let out = Point { x: v.x + a.bias, y: v.y + a.bias };\n"
                        "        return out;\n"
                        "    }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let a = Adder { bias: 10 };\n"
                        "    let v = Point { x: 1, y: 2 };\n"
                        "    let out: Point = a.add(v);\n"
                        "    return out.x * 100 + out.y;\n"
                        "}\n"
                        "let r: i32 = main();") == 1112);
}

static void test_call_on_a_nested_struct() {
    assert(test_run_int("struct Inner { n: i32 }\n"
                        "struct Outer { inner: Inner }\n"
                        "impl Inner {\n"
                        "    func bump(i: &Inner): i32 { i.n = i.n + 1; return i.n; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let o = Outer { inner: Inner { n: 5 } };\n"
                        "    let ignored: i32 = o.inner.bump();\n"
                        "    return o.inner.n;\n"
                        "}\n"
                        "let r: i32 = main();") == 6);
}

static void test_call_diagnostics() {
    assert(fails("struct Player { health: i32 }\n"
                 "func main(): i32 { let p = Player { health: 0 }; return p.nope(); }\n"));

    assert(fails("struct Player { health: i32 }\n"
                 "impl Player {\n"
                 "    func hp(p: &Player): i32 { return p.health; }\n"
                 "}\n"
                 "func main(): i32 { let p = Player { health: 0 }; return p.hp(1); }\n"));

    assert(fails("struct Player { health: i32 }\n"
                 "impl Player {\n"
                 "    func set(p: &Player, n: i32): i32 { return n; }\n"
                 "}\n"
                 "func main(): i32 { let p = Player { health: 0 }; return p.set(true); }\n"));

    assert(fails("struct Player { health: i32 }\n"
                 "impl Player {\n"
                 "    func hp(p: &Player): i32 { return p.health; }\n"
                 "}\n"
                 "func make(): Player { let p = Player { health: 0 }; return p; }\n"
                 "func main(): i32 { return make().hp(); }\n"));

    assert(fails("func main(): i32 { let n: i32 = 1; return n.hp(); }\n"));
}

static void test_a_matching_receiver_needs_no_adjustment() {
    assert(test_run_int("struct Box { n: i32 }\n"
                        "impl Box {\n"
                        "    func get(b: &Box): i32 { return b.n; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let b: *Box = box Box { n: 0 };\n"
                        "    b.n = 7;\n"
                        "    return b.get();\n"
                        "}\n"
                        "let r: i32 = main();") == 7);
}

static void test_a_value_receiver_has_its_address_taken() {
    assert(test_run_int("struct Box { n: i32 }\n"
                        "impl Box {\n"
                        "    func bump(b: &Box): i32 { b.n = b.n + 1; return b.n; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let b = Box { n: 4 };\n"
                        "    let got: i32 = b.bump();\n"
                        "    return got * 10 + b.n;\n"
                        "}\n"
                        "let r: i32 = main();") == 55);
}

static void test_a_pointer_receiver_is_dereferenced() {
    assert(test_run_int("struct Box { n: i32 }\n"
                        "impl Box {\n"
                        "    func peek(b: Box): i32 { return b.n; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let b: *Box = box Box { n: 0 };\n"
                        "    b.n = 9;\n"
                        "    return b.peek();\n"
                        "}\n"
                        "let r: i32 = main();") == 9);
}

static void test_a_method_on_a_ref_receiver() {
    assert(test_run_int("struct Box { n: i32 }\n"
                        "impl Box {\n"
                        "    func get(b: &Box): i32 { return b.n; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let owner: *Box = box Box { n: 0 };\n"
                        "    owner.n = 6;\n"
                        "    let borrowed: &Box = owner;\n"
                        "    return borrowed.get();\n"
                        "}\n"
                        "let r: i32 = main();") == 6);
}

static void test_a_method_through_a_ref_mutates_the_owned_object() {
    assert(test_run_int("struct Box { n: i32 }\n"
                        "impl Box {\n"
                        "    func bump(b: &Box): i32 { b.n = b.n + 1; return b.n; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let owner: *Box = box Box { n: 0 };\n"
                        "    owner.n = 4;\n"
                        "    let borrowed: &Box = owner;\n"
                        "    borrowed.bump();\n"
                        "    return owner.n;\n"
                        "}\n"
                        "let r: i32 = main();") == 5);
}

static void test_a_receiver_two_levels_out_reaches_the_method() {
    assert(test_run_int("struct Box { n: i32 }\n"
                        "impl Box {\n"
                        "    func get(b: &Box): i32 { return b.n; }\n"
                        "}\n"
                        "func main(): i32 {\n"
                        "    let owner: *Box = box Box { n: 0 };\n"
                        "    owner.n = 7;\n"
                        "    let outer: &*Box = owner;\n"
                        "    return outer.get();\n"
                        "}\n"
                        "let r: i32 = main();") == 7);
}

static void test_a_consuming_function_takes_the_receiver() {
    assert(test_compiles("struct Box { n: i32 }\n"
                         "impl Box {\n"
                         "    func peek(b: *Box): i32 { return b.n; }\n"
                         "}\n"));

    assert(test_compiles("struct Box { n: i32 }\n"
                         "impl Box {\n"
                         "    func peek(b: *Box): i32 { return b.n; }\n"
                         "}\n"
                         "func main(): i32 {\n"
                         "    let a: *Box = box Box { n: 0 };\n"
                         "    return a.peek();\n"
                         "}\n"));

    assert(!test_compiles("struct Box { n: i32 }\n"
                          "impl Box {\n"
                          "    func peek(b: *Box): i32 { return b.n; }\n"
                          "}\n"
                          "func main(): i32 {\n"
                          "    let a: *Box = box Box { n: 0 };\n"
                          "    a.peek();\n"
                          "    return a.n;\n"
                          "}\n"));
}

static void test_a_pointer_method_on_a_temporary_is_refused() {
    assert(!test_compiles("struct Box { n: i32 }\n"
                          "impl Box {\n"
                          "    func get(b: &Box): i32 { return b.n; }\n"
                          "}\n"
                          "func make(): Box { let b = Box { n: 0 }; return b; }\n"
                          "func main(): i32 { return make().get(); }\n"));
}

static void test_arity_errors_exclude_the_receiver() {
    assert(!test_compiles("struct Box { n: i32 }\n"
                          "impl Box {\n"
                          "    func add(b: &Box, x: i32): i32 { return b.n + x; }\n"
                          "}\n"
                          "func main(): i32 {\n"
                          "    let b: *Box = box Box { n: 0 };\n"
                          "    return b.add(1, 2);\n"
                          "}\n"));
}

static void test_an_unknown_method_is_refused() {
    assert(!test_compiles("struct Box { n: i32 }\n"
                          "func main(): i32 {\n"
                          "    let b: *Box = box Box { n: 0 };\n"
                          "    return b.nope();\n"
                          "}\n"));
}

static void test_parameter_zero_takes_the_form_it_declares() {
    assert(test_compiles("struct Node { child: *Node }\n"
                         "impl Node {\n"
                         "    func peek(n: Node): i32 { return 0; }\n"
                         "}\n"));

    assert(test_compiles("struct Node { child: *Node }\n"
                         "impl Node {\n"
                         "    func peek(n: &Node): i32 { return 0; }\n"
                         "}\n"));

    assert(test_compiles("struct Point { x: i32, y: i32 }\n"
                         "impl Point {\n"
                         "    func peek(p: Point): i32 { return p.x; }\n"
                         "}\n"));

    assert(!test_compiles("struct Node { child: *Node }\n"
                          "impl Node {\n"
                          "    func peek(n: Node): i32 { return 0; }\n"
                          "}\n"
                          "func main(): i32 { let a: Node; a.peek(); return a.peek(); }\n"));
}

static void test_a_pointer_receiver_is_not_reached_from_another_type() {
    assert(!test_compiles("struct Box { n: i32 }\n"
                          "impl Box {\n"
                          "    func take(self: *Self): i32 { return self.n; }\n"
                          "}\n"
                          "func f(): i32 { let b: &Box = box Box { n: 1 }; return b.take(); }\n"));

    assert(test_compiles("struct Box { n: i32 }\n"
                         "impl Box {\n"
                         "    func peek(self: &Self): i32 { return self.n; }\n"
                         "}\n"
                         "func f(): i32 { let b: *Box = box Box { n: 1 }; return b.peek(); }\n"));
}

int main(void) {
    test_a_matching_receiver_needs_no_adjustment();
    test_a_value_receiver_has_its_address_taken();
    test_a_pointer_receiver_is_dereferenced();
    test_a_method_on_a_ref_receiver();
    test_a_method_through_a_ref_mutates_the_owned_object();
    test_a_receiver_two_levels_out_reaches_the_method();
    test_a_consuming_function_takes_the_receiver();
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
