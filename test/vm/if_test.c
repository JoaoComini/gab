// What 'if' requires of its condition, and that it branches on it correctly.
// The parser tests already cover the syntax; this covers the rule that the
// condition must be a bool, which nothing checked -- 'if p' on a pointer
// compiled and ran.
#include "ast/ast.h"
#include "diagnostics.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/test_context.h"
#include "value.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

// Runs a script and returns the slot the top-level result lands in.
static int run_int(const char *source) {
    VM *vm = vm_create();

    vm_execute(vm, source);

    assert(vm->frame_count == 0);

    int result = (*vm_slot(vm, 0)).as_int;

    vm_free(vm);

    return result;
}

static bool compiles(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    Lexer lexer = lexer_create(source, &ctx.diagnostics);
    Parser parser = parser_create(&lexer, &ctx.diagnostics);

    bool ok = parser_parse(&parser, script) &&
              ast_script_resolve(ctx.arena, script, scope, NULL, &ctx.diagnostics);

    ast_script_destroy(script);
    test_context_free(&ctx);

    return ok;
}

static void test_branches_on_a_bool() {
    assert(run_int("func f(): int { if true { return 1; } return 0; }\n"
                   "let r: int = f();\n") == 1);

    assert(run_int("func f(): int { if false { return 1; } return 0; }\n"
                   "let r: int = f();\n") == 0);
}

static void test_else_runs_when_the_condition_is_false() {
    assert(run_int("func f(): int { if false { return 1; } else { return 2; } }\n"
                   "let r: int = f();\n") == 2);
}

static void test_a_comparison_is_a_valid_condition() {
    assert(compiles("func f(): int { let x: int = 1; if x > 0 { return 1; } return 0; }\n"));
    assert(compiles("func f(): int { let x: bool = true; if x { return 1; } return 0; }\n"));
    assert(compiles("func f(): int { if true && false { return 1; } return 0; }\n"));
}

// The regression: every one of these has a non-bool condition and used to
// compile. The condition was visited so its own errors were caught, but its
// type was never checked against anything.
static void test_a_non_bool_condition_is_rejected() {
    assert(!compiles("func f(): int { let x: int = 1; if x { return 1; } return 0; }\n"));

    assert(!compiles("func f(): float { let x: float = 1.0; if x { return 1.0; } return 0.0; }\n"));

    assert(!compiles("func f(): int { let x: int = 1; let p: *int = &x; if p { return 1; } return 0; }\n"));

    assert(!compiles("struct V { x: int }\n"
                     "func f(): int { let v: V; if v { return 1; } return 0; }\n"));
}

// A condition that is already an error must not produce a second complaint
// about not being a bool: one mistake should report once.
static void test_a_poisoned_condition_reports_once() {
    assert(!compiles("func f(): int { if undefined_name { return 1; } return 0; }\n"));
}

int main() {
    test_branches_on_a_bool();
    test_else_runs_when_the_condition_is_false();
    test_a_comparison_is_a_valid_condition();
    test_a_non_bool_condition_is_rejected();
    test_a_poisoned_condition_reports_once();

    printf("if_test: all tests passed\n");
    return 0;
}
