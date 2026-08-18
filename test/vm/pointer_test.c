#include "ast/ast.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/test_context.h"
#include "type.h"
#include "type_registry.h"
#include "vm/chunk.h"
#include "vm/codegen.h"
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

    return ast_script_resolve(ctx->arena, script, scope, NULL, NULL, &ctx->diagnostics);
}

static Symbol *lookup(TestContext *ctx, Scope *scope, const char *name) {
    return scope_symbol_lookup(scope, string_from_cstr(&ctx->strings, name));
}

// The symbol of the first pointer-typed local declared in a function body. A
// function scope is gone by the time resolution finishes, so the symbol is read
// back off the declaration rather than looked up by name.
static Symbol *pointer_symbol(const ASTStmt *func) {
    assert(func->kind == STMT_FUNC_DECL);

    const ASTStmtList *body = &func->func_decl.body->block.list;

    for (int i = 0; i < body->size; i++) {
        ASTStmt *stmt = body->data[i];

        if (stmt->kind == STMT_VAR_DECL && type_is_pointer(stmt->var_decl.symbol->var.type)) {
            return stmt->var_decl.symbol;
        }
    }

    return NULL;
}

// The whole type system compares types by pointer identity, so two mentions of
// '*Player' must yield the same Type *.
static void test_pointer_types_are_interned() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script,
                        "struct Player { health: int }\n"
                        "let p: *Player;\n"
                        "let q: *Player;\n");
    assert(ok);

    Symbol *p = lookup(&ctx, scope, "p");
    Symbol *q = lookup(&ctx, scope, "q");

    assert(p && q);
    assert(p->var.type == q->var.type);
    assert(type_is_pointer(p->var.type));

    Type *player = scope_type_lookup(scope, string_from_cstr(&ctx.strings, "Player"));
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

    bool ok = resolve(&ctx, scope, script, "let p: *int;\nlet q: **int;\n");
    assert(ok);

    Symbol *p = lookup(&ctx, scope, "p");
    Symbol *q = lookup(&ctx, scope, "q");

    assert(q->var.type->pointee == p->var.type);
    assert(p->var.type->pointee ==
           scope_type_lookup(scope, string_from_cstr(&ctx.strings, "int")));

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

    bool ok = resolve(&ctx, scope, script,
                        "struct Big { a: int, b: int, c: int, d: int }\n"
                        "let p: *Big;\n"
                        "let q: *bool;\n");
    assert(ok);

    Symbol *p = lookup(&ctx, scope, "p");
    Symbol *q = lookup(&ctx, scope, "q");

    assert(p->var.type->size == sizeof(void *));
    assert(p->var.type->alignment == _Alignof(void *));
    assert(q->var.type->size == p->var.type->size);
    assert(q->var.type->alignment == p->var.type->alignment);

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// The address is a real address into the stack, so writing through it must be
// visible to the variable itself.
static void test_scalar_read_and_write_through_a_pointer() {
    assert(run_int("func f(): int { let x: int = 7; let p: *int = &x; return *p; }\n"
                        "let r: int = f();") == 7);

    assert(run_int("func f(): int { let x: int = 3; let p: *int = &x; *p = 42; return x; }\n"
                        "let r: int = f();") == 42);

    assert(run_float("func f(): float { let x: float = 1.5; let p: *float = &x; *p = 2.5; return x; }\n"
                     "let r: float = f();") == 2.5f);
}

// A field write through a pointer must land in the pointee and disturb nothing
// beside it.
static void test_field_write_through_a_pointer() {
    assert(run_int("struct Player { health: int, mana: int }\n"
                        "func f(): int { let p: Player; p.health = 1; p.mana = 2;\n"
                        "let q: *Player = &p; (*q).health = 10;\n"
                        "return p.health * 100 + p.mana; }\n"
                        "let r: int = f();") == 1002);
}

// A pointer to a field addresses that field alone, so writing through it must
// not touch its neighbours.
static void test_pointer_to_a_struct_field() {
    assert(run_int("struct Vec { x: int, y: int }\n"
                        "func f(): int { let v: Vec; v.x = 1; v.y = 2;\n"
                        "let p: *int = &v.y; *p = 9;\n"
                        "return v.x * 100 + v.y; }\n"
                        "let r: int = f();") == 109);
}

// Sub-word fields share a slot, so a pointer to one must still write only its
// own byte.
static void test_pointer_to_a_sub_word_field() {
    assert(run_int("struct Flags { a: bool, b: bool, c: bool, d: bool }\n"
                        "func f(): int { let v: Flags;\n"
                        "v.a = true; v.b = true; v.c = true; v.d = true;\n"
                        "let p: *bool = &v.b; *p = false;\n"
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
    assert(run_int("struct Vec { x: int, y: int }\n"
                        "func f(): int { let v: Vec; v.x = 3; v.y = 4;\n"
                        "let p: *Vec = &v;\n"
                        "let copy: Vec = *p;\n"
                        "v.x = 100;\n"
                        "return copy.x * 10 + copy.y; }\n"
                        "let r: int = f();") == 34);
}

// A pointer to a pointer still resolves to one address at the end of the chain.
static void test_pointer_to_a_pointer() {
    assert(run_int("func f(): int { let x: int = 5;\n"
                        "let p: *int = &x;\n"
                        "let q: **int = &p;\n"
                        "**q = 11;\n"
                        "return x; }\n"
                        "let r: int = f();") == 11);
}

// Addresses point into a buffer that realloc may move. Recursing deep enough to
// force the growth while a pointer to an outer frame is live is what catches a
// missing rebase.
static void test_a_pointer_survives_a_stack_growth() {
    assert(run_int("func deep(n: int, p: *int): int {\n"
                        "if n > 0 { return deep(n - 1, p); }\n"
                        "return *p;\n"
                        "}\n"
                        "func f(): int { let x: int = 77; return deep(200, &x); }\n"
                        "let r: int = f();") == 77);

    // And writing through it, so the frame the address came from is what
    // actually changed.
    assert(run_int("func deep(n: int, p: *int): int {\n"
                        "if n > 0 { return deep(n - 1, p); }\n"
                        "*p = 88;\n"
                        "return 0;\n"
                        "}\n"
                        "func f(): int { let x: int = 1; let ignored: int = deep(200, &x); return x; }\n"
                        "let r: int = f();") == 88);
}

// 'p.health' where p is a '*Player' reaches through the pointer, the way Go
// and C's '->' do, so a '*T' receiver will read naturally in step 5.
static void test_field_access_auto_derefs() {
    assert(run_int("struct Player { health: int, mana: int }\n"
                        "func f(): int { let p: Player; p.health = 1; p.mana = 2;\n"
                        "let q: *Player = &p; q.health = 10;\n"
                        "return p.health * 100 + p.mana; }\n"
                        "let r: int = f();") == 1002);

    assert(run_int("struct Player { health: int, mana: int }\n"
                        "func f(): int { let p: Player; p.health = 10;\n"
                        "let q: *Player = &p; return q.health; }\n"
                        "let r: int = f();") == 10);
}

// Auto-deref composes with the inline layout of a nested struct: one address
// plus a summed offset still reaches the innermost field.
static void test_auto_deref_reaches_a_nested_field() {
    assert(run_int("struct Inner { v: int }\n"
                        "struct Outer { a: int, inner: Inner }\n"
                        "func f(): int { let o: Outer; let q: *Outer = &o;\n"
                        "q.inner.v = 7; return o.inner.v; }\n"
                        "let r: int = f();") == 7);
}

// Taking the address of a field reached through a pointer adds the offset to
// the address rather than to a slot index.
static void test_address_of_a_field_through_a_pointer() {
    assert(run_int("struct Player { health: int, mana: int }\n"
                        "func f(): int { let p: Player; p.mana = 2;\n"
                        "let q: *Player = &p;\n"
                        "let h: *int = &q.health; *h = 9;\n"
                        "return p.health * 100 + p.mana; }\n"
                        "let r: int = f();") == 902);
}

// An 8-byte pointer needs an even slot index to sit at its natural alignment.
static void test_a_pointer_local_is_slot_aligned() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    // The odd number of leading scalars is the point: without alignment the
    // pointer would land on an odd slot.
    bool ok = resolve(&ctx, scope, script,
                        "func f(): int {\n"
                        "let a: bool = true;\n"
                        "let x: int = 1;\n"
                        "let p: *int = &x;\n"
                        "return *p;\n"
                        "}\n");
    assert(ok);

    FuncProtoList global_funcs = func_proto_list_create();
    Chunk *chunk = codegen_generate(script, &global_funcs, &ctx.diagnostics, NULL);
    assert(chunk);

    Symbol *p = pointer_symbol(script->statements.data[0]);
    assert(p);
    assert(p->offset % VM_POINTER_SLOTS == 0);

    chunk_free(chunk);
    func_proto_list_free(&global_funcs);
    ast_script_destroy(script);
    test_context_free(&ctx);
}

int main() {
    test_pointer_types_are_interned();
    test_pointer_depth_nests();
    test_pointer_is_a_word();
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
    test_a_pointer_local_is_slot_aligned();

    printf("pointer_test: all tests passed\n");
    return 0;
}
