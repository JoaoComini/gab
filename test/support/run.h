#ifndef GAB_TEST_RUN_H
#define GAB_TEST_RUN_H

// Running a snippet of script and asking what came back. Every feature test
// wants some version of this, so it lives here rather than being copied: a
// change to what "ran correctly" means -- an extra leak assertion, say --
// should have exactly one place to be made.
//
// Header-only and 'static inline', matching test_context.h. The tests are
// separate executables with no shared translation unit to link against.

#include "ast/ast.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "support/test_context.h"
#include "value.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>

// Runs a script and returns the slot the top-level result lands in.
//
// The frame-count assertion is the shared correctness check: a script that
// returned normally has unwound every frame, so a non-zero count means the run
// went wrong in a way the returned value alone would not show.
static inline Value test_run(const char *source) {
    VM *vm = vm_create();

    vm_execute(vm, source);

    assert(vm->frame_count == 0);

    Value result = (*vm_slot(vm, 0));

    vm_free(vm);

    return result;
}

static inline int test_run_int(const char *source) { return test_run(source).as_int; }

static inline float test_run_float(const char *source) { return test_run(source).as_float; }

static inline bool test_run_bool(const char *source) { return test_run(source).as_int != 0; }

// Compiles as far as resolution and leaves the results in the caller's scope
// and script, so a test can inspect the symbols and types the front end
// settled on. The caller owns both, since what is worth inspecting differs.
static inline bool test_resolve(TestContext *ctx, Scope *scope, ASTScript *script, const char *source) {
    Lexer lexer = lexer_create(source, &ctx->diagnostics);
    Parser parser = parser_create(&lexer, &ctx->diagnostics);

    if (!parser_parse(&parser, script)) {
        return false;
    }

    return ast_script_resolve(ctx->arena, script, scope, NULL, &ctx->diagnostics);
}

// Whether a source compiles at all, for the cases that only care that a bad
// program is rejected. Owns its own context, since nothing is inspected after.
static inline bool test_compiles(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTScript *script = ast_script_create();

    bool ok = test_resolve(&ctx, scope, script, source);

    ast_script_destroy(script);
    test_context_free(&ctx);

    return ok;
}

// Runs a script expected to fail, and returns why. Compilation must still
// succeed: this is for runtime traps, where the mistake is only visible once
// the offending instruction executes.
static inline VmRunStatus test_run_status(const char *source) {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->compile_arena, "<test>");

    CompiledScript script;
    bool compiled = vm_compile(vm, source, &script, &diagnostics);

    diagnostics_free(&diagnostics);
    assert(compiled);

    VmRunStatus status = vm_run(vm, &script);

    // The status is reported both ways round, and a failure always carries a
    // message for the host to show.
    assert(vm->error.status == status);
    assert(status == VM_RUN_OK || vm->error.message);

    vm_compiled_script_free(&script);
    vm_free(vm);

    return status;
}

#endif
