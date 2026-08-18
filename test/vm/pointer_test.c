#include "ast/ast.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/test_context.h"
#include "type.h"
#include "type_registry.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Runs a script and returns the slot the top-level result lands in.
static Value run(const char *source) {
    VM *vm = vm_create();

    vm_execute(vm, source);

    assert(vm->frame_count == 0);

    Value result = (*vm_slot(vm, 0));

    vm_free(vm);

    return result;
}

static int run_int(const char *source) { return run(source).as_int; }

static float run_float(const char *source) { return run(source).as_float; }

// Compiles as far as resolution and hands back the scope, so a test can inspect
// the symbols and types the front end settled on.
static bool resolve(TestContext *ctx, Scope *scope, ASTScript *script, const char *source) {
    Lexer lexer = lexer_create(source, &ctx->diagnostics);
    Parser parser = parser_create(&lexer, &ctx->diagnostics);

    if (!parser_parse(&parser, script)) {
        return false;
    }

    return ast_script_resolve(ctx->arena, script, scope, &ctx->diagnostics);
}

static Symbol *lookup(TestContext *ctx, Scope *scope, const char *name) {
    return scope_symbol_lookup(scope, string_from_cstr(&ctx->strings, name));
}

// The whole type system compares types by pointer identity, so two mentions of
// '*Player' must yield the same Type *.
static void test_pointer_types_are_interned() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    assert(resolve(&ctx, scope, script,
                   "struct Player { health: int }\n"
                   "let p: *Player;\n"
                   "let q: *Player;\n"));

    Symbol *p = lookup(&ctx, scope, "p");
    Symbol *q = lookup(&ctx, scope, "q");

    assert(p && q);
    assert(p->var.type == q->var.type);
    assert(type_is_pointer(p->var.type));

    Type *player = type_registry_get(scope->type_registry, string_from_cstr(&ctx.strings, "Player"));
    assert(p->var.type->pointee == player);

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// '**T' is a pointer to the interned '*T', not a second flavour of pointer.
static void test_pointer_depth_nests() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    assert(resolve(&ctx, scope, script, "let p: *int;\nlet q: **int;\n"));

    Symbol *p = lookup(&ctx, scope, "p");
    Symbol *q = lookup(&ctx, scope, "q");

    assert(q->var.type->pointee == p->var.type);
    assert(p->var.type->pointee ==
           type_registry_get(scope->type_registry, string_from_cstr(&ctx.strings, "int")));

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// A pointer is a raw address: 8 bytes wanting 8-byte alignment, whatever it
// points at.
static void test_pointer_is_a_word() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    assert(resolve(&ctx, scope, script,
                   "struct Big { a: int, b: int, c: int, d: int }\n"
                   "let p: *Big;\n"
                   "let q: *bool;\n"));

    Symbol *p = lookup(&ctx, scope, "p");
    Symbol *q = lookup(&ctx, scope, "q");

    assert(p->var.type->size == sizeof(void *));
    assert(p->var.type->alignment == _Alignof(void *));
    assert(q->var.type->size == p->var.type->size);
    assert(q->var.type->alignment == p->var.type->alignment);

    ast_script_destroy(script);
    test_context_free(&ctx);
}

int main() {
    test_pointer_types_are_interned();
    test_pointer_depth_nests();
    test_pointer_is_a_word();

    printf("pointer_test: all tests passed\n");
    return 0;
}
