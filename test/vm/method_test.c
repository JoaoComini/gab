#include "ast/ast.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/test_context.h"
#include "type.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

// Compiles as far as resolution and hands back the scope, so a test can inspect
// the types and method tables the front end settled on.
static bool resolve(TestContext *ctx, Scope *scope, ASTScript *script, const char *source) {
    Lexer lexer = lexer_create(source, &ctx->diagnostics);
    Parser parser = parser_create(&lexer, &ctx->diagnostics);

    if (!parser_parse(&parser, script)) {
        return false;
    }

    return ast_script_resolve(ctx->arena, script, scope, NULL, NULL, &ctx->diagnostics);
}

static Type *lookup_type(TestContext *ctx, Scope *scope, const char *name) {
    return scope_type_lookup(scope, string_from_cstr(&ctx->strings, name));
}

static Symbol *lookup_method(TestContext *ctx, Scope *scope, const char *type, const char *method) {
    return type_find_method(lookup_type(ctx, scope, type), string_from_cstr(&ctx->strings, method));
}

// The receiver clause declares the function on the type rather than in a scope.
static void test_method_lands_on_its_receiver_type() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script,
                      "struct Player { health: int }\n"
                      "func (p: *Player) damage(n: int): bool { return true; }\n");
    assert(ok);

    Symbol *damage = lookup_method(&ctx, scope, "Player", "damage");

    assert(damage);
    assert(damage->kind == SYMBOL_FUNC);

    // The receiver is parameter zero, so a one-parameter method has two.
    assert(damage->func.param_count == 2);
    assert(type_is_pointer(damage->func.params[0]));
    assert(damage->func.params[0]->pointee == lookup_type(&ctx, scope, "Player"));

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// A method has no free-standing name: 'damage' alone must not resolve, or it
// would collide with a user's own declaration.
static void test_method_is_not_reachable_as_a_bare_name() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script,
                      "struct Player { health: int }\n"
                      "func (p: *Player) damage(n: int): bool { return true; }\n");
    assert(ok);

    assert(!scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "damage")));

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// The whole reason methods are keyed by type: two structs may each declare an
// 'update', and the two are different functions.
static void test_same_name_on_two_types() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script,
                      "struct Player { health: int }\n"
                      "struct Enemy { health: int }\n"
                      "func (p: *Player) update(): int { return 1; }\n"
                      "func (e: *Enemy) update(): int { return 2; }\n");
    assert(ok);

    Symbol *player = lookup_method(&ctx, scope, "Player", "update");
    Symbol *enemy = lookup_method(&ctx, scope, "Enemy", "update");

    assert(player && enemy && player != enemy);

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// A value receiver is the struct itself, not a pointer to it.
static void test_value_receiver() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script,
                      "struct Player { health: int }\n"
                      "func (p: Player) is_alive(): bool { return true; }\n");
    assert(ok);

    Symbol *alive = lookup_method(&ctx, scope, "Player", "is_alive");

    assert(alive);
    assert(alive->func.param_count == 1);
    assert(alive->func.params[0] == lookup_type(&ctx, scope, "Player"));

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// Declarations are hoisted, so a method may precede the struct it receives.
static void test_method_declared_above_its_struct() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script,
                      "func (p: *Player) health_of(): int { return p.health; }\n"
                      "struct Player { health: int }\n");
    assert(ok);

    assert(lookup_method(&ctx, scope, "Player", "health_of"));

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// The receiver is an ordinary local in the body, so its fields resolve — and
// through a pointer receiver that means the existing auto-deref.
static void test_receiver_fields_resolve_in_the_body() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script,
                      "struct Player { health: int }\n"
                      "func (p: *Player) hp(): int { return p.health; }\n"
                      "func (q: Player) hp2(): int { return q.health; }\n");
    assert(ok);

    ast_script_destroy(script);
    test_context_free(&ctx);
}

static bool fails(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script, source);

    ast_script_destroy(script);
    test_context_free(&ctx);

    return !ok;
}

static void test_diagnostics() {
    // One type may not declare the same method twice.
    assert(fails("struct Player { health: int }\n"
                 "func (p: *Player) update(): int { return 1; }\n"
                 "func (p: *Player) update(): int { return 2; }\n"));

    // A receiver has to be a struct; 'int' has no method set to hang one on.
    assert(fails("func (n: int) double(): int { return n; }\n"));

    // Nor a type that does not exist at all.
    assert(fails("func (p: *Missing) update(): int { return 1; }\n"));
}

int main(void) {
    test_method_lands_on_its_receiver_type();
    test_method_is_not_reachable_as_a_bare_name();
    test_same_name_on_two_types();
    test_value_receiver();
    test_method_declared_above_its_struct();
    test_receiver_fields_resolve_in_the_body();
    test_diagnostics();

    printf("All method tests passed\n");
    return 0;
}
