#include "ast/ast.h"
#include "diagnostics.h"
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

static int run_int(const char *source) {
    VM *vm = vm_create();

    vm_execute(vm, source);

    assert(vm->frame_count == 0);

    int result = (*vm_slot(vm, 0)).as_int;

    vm_free(vm);

    return result;
}

static bool resolve(TestContext *ctx, Scope *scope, ASTScript *script, const char *source) {
    Lexer lexer = lexer_create(source, &ctx->diagnostics);
    Parser parser = parser_create(&lexer, &ctx->diagnostics);

    if (!parser_parse(&parser, script)) {
        return false;
    }

    return ast_script_resolve(ctx->arena, script, scope, NULL, &ctx->diagnostics);
}

static bool compiles(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script, source);

    ast_script_destroy(script);
    test_context_free(&ctx);

    return ok;
}

// 'weak *T' and '*T' are different types, so that codegen can tell from a
// field's type alone whether releasing it touches the strong count.
static void test_weak_is_a_distinct_type() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script,
                      "struct Node { n: int }\n"
                      "let s: *Node;\n"
                      "let w: weak *Node;\n");
    assert(ok);

    Symbol *strong = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "s"));
    Symbol *weak = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "w"));

    assert(strong && weak);
    assert(strong->var.type != weak->var.type);
    assert(!strong->var.type->is_weak);
    assert(weak->var.type->is_weak);

    // Same pointee, and both are still ordinary pointers: a weak reference is
    // the same address, differing only in the count it touches.
    assert(strong->var.type->pointee == weak->var.type->pointee);
    assert(weak->var.type->size == sizeof(void *));

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// Interned like every other type, so two mentions of 'weak *Node' are one Type.
static void test_weak_pointers_are_interned() {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = resolve(&ctx, scope, script,
                      "struct Node { n: int }\n"
                      "let a: weak *Node;\n"
                      "let b: weak *Node;\n");
    assert(ok);

    Symbol *a = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "a"));
    Symbol *b = scope_symbol_lookup(scope, string_from_cstr(&ctx.strings, "b"));

    assert(a->var.type == b->var.type);

    ast_script_destroy(script);
    test_context_free(&ctx);
}

// The shape a scene graph is made of, and the reason weak references exist: a
// child knows its parent without keeping it alive.
static void test_a_self_referential_struct_declares() {
    assert(compiles("struct Node { parent: weak *Node, child: *Node }\n"));

    // A pointer to self is not containment, weak or strong.
    assert(compiles("struct Node { child: *Node }\n"));

    // Containing itself by value still is.
    assert(!compiles("struct Node { self: Node }\n"));
}

// Weakening never fails, so a strong reference may be stored where a weak one
// is expected. The reverse can fail — the object may be gone — and needs a way
// to report that, so it is rejected until there is one.
static void test_conversion_is_strong_to_weak_only() {
    assert(compiles("struct Node { n: int }\n"
                    "func f(): int { let s: *Node = new Node; let w: weak *Node = s; return 0; }\n"));

    assert(!compiles("struct Node { n: int }\n"
                     "func f(): int { let w: weak *Node; let s: *Node = w; return 0; }\n"));
}

// Weakness qualifies a reference, and a value is not one.
static void test_weak_requires_a_pointer() {
    assert(!compiles("struct Node { n: int }\nlet x: weak Node;\n"));
}

// A weak field is still a field: it reads and writes like any other pointer,
// since the address is identical and only the counting differs.
static void test_a_weak_field_reads_and_writes() {
    assert(run_int("struct Node { n: int, parent: weak *Node }\n"
                   "func main(): int {\n"
                   "    let a: *Node = new Node;\n"
                   "    let b: *Node = new Node;\n"
                   "    a.n = 7;\n"
                   "    b.parent = a;\n"
                   "    return b.parent.n;\n"
                   "}\n"
                   "let r: int = main();") == 7);
}

// The whole point of the step, and the shape a scene graph actually is: a
// child holds its parent strongly-not, and both ends free. With two strong
// references this leaks both nodes forever.
//
// Correctness here is that it runs clean under LeakSanitizer, which is what the
// suite is built with — the returned value only proves the graph was walkable.
static void test_a_parent_child_cycle_frees_both() {
    assert(run_int("struct Node { n: int, parent: weak *Node, child: *Node }\n"
                   "func main(): int {\n"
                   "    let root: *Node = new Node;\n"
                   "    root.n = 1;\n"
                   "    root.child = new Node;\n"
                   "    root.child.n = 2;\n"
                   "    root.child.parent = root;\n"
                   "    return root.child.parent.n + root.child.n;\n"
                   "}\n"
                   "let r: int = main();") == 3);
}

// Reaching through a weak reference whose object is gone fails the run rather
// than reading the zeroed payload, which would answer plausibly instead of
// obviously wrongly.
static void test_a_dead_weak_deref_traps() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, "<test>");

    CompiledScript script;
    assert(vm_compile(vm,
                      "struct Node { n: int }\n"
                      "func main(): int {\n"
                      "    let w: weak *Node;\n"
                      "    { let owner: *Node = new Node; owner.n = 7; w = owner; }\n"
                      "    return w.n;\n"
                      "}\n"
                      "let r: int = main();",
                      &script, &diagnostics));

    diagnostics_free(&diagnostics);

    assert(vm_run(vm, &script) == VM_RUN_ERR_DANGLING_WEAK);
    assert(vm->error.status == VM_RUN_ERR_DANGLING_WEAK);
    assert(vm->error.message);

    vm_compiled_script_free(&script);
    vm_free(vm);
}

// A weak reference whose object is still alive reaches through normally: the
// check is a guard, not a tax on every use.
static void test_a_live_weak_deref_works() {
    assert(run_int("struct Node { n: int }\n"
                   "func main(): int {\n"
                   "    let owner: *Node = new Node;\n"
                   "    owner.n = 12;\n"
                   "    let w: weak *Node = owner;\n"
                   "    return w.n;\n"
                   "}\n"
                   "let r: int = main();") == 12);
}

// A failure unwinds past every release codegen emitted, so the frames drop what
// they hold on the way out. Nothing weak about this one — it is the same path,
// and it leaked from the moment refcounting landed until the unwind learned to
// release.
static void test_an_abnormal_unwind_releases_what_it_held() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, "<test>");

    CompiledScript script;
    assert(vm_compile(vm,
                      "struct Node { n: int }\n"
                      "func deep(n: int): int { return deep(n + 1); }\n"
                      "func main(): int { let p: *Node = new Node; return deep(0); }\n"
                      "let r: int = main();",
                      &script, &diagnostics));

    diagnostics_free(&diagnostics);

    assert(vm_run(vm, &script) == VM_RUN_ERR_CALL_DEPTH);

    vm_compiled_script_free(&script);
    vm_free(vm);
}

int main(void) {
    test_weak_is_a_distinct_type();
    test_weak_pointers_are_interned();
    test_a_self_referential_struct_declares();
    test_conversion_is_strong_to_weak_only();
    test_weak_requires_a_pointer();
    test_a_weak_field_reads_and_writes();
    test_a_parent_child_cycle_frees_both();
    test_a_dead_weak_deref_traps();
    test_a_live_weak_deref_works();
    test_an_abnormal_unwind_releases_what_it_held();

    printf("All weak tests passed\n");
    return 0;
}
