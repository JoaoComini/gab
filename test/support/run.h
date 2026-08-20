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
#include "vm/chunk.h"
#include "vm/opcode.h"
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

// A compiled program, held open so its instructions can be inspected. The VM
// stays alive because the function prototypes live on it, not on the script.
//
// For the codegen tests, which ask what code was emitted rather than what it
// computes. Everything else should prefer test_run_int and friends: what a
// program does is the durable claim, and the instructions that achieve it are
// free to change.
typedef struct {
    VM *vm;
    CompiledScript script;
} TestProgram;

static inline TestProgram test_compile(const char *source) {
    TestProgram program = {.vm = vm_create()};

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, program.vm->compile_arena, "<test>");

    bool ok = vm_compile(program.vm, source, &program.script, &diagnostics);

    diagnostics_free(&diagnostics);
    assert(ok);

    return program;
}

static inline void test_program_free(TestProgram *program) {
    vm_compiled_script_free(&program->script);
    vm_free(program->vm);
}

// The top-level chunk: what the script's own statements compiled to.
static inline Chunk *test_top_chunk(TestProgram *program) { return program->script.chunk; }

// The chunk of a declared function, by declaration order.
static inline Chunk *test_func_chunk(TestProgram *program, size_t index) {
    assert(index < program->vm->global_funcs.size);

    return program->vm->global_funcs.data[index].chunk;
}

// How many times an opcode appears. Most claims about emitted code are really
// counts -- "one move, not two", "no MOVE_N at all" -- and a count says that
// without pinning where in the chunk it landed.
static inline size_t test_count_opcode(const Chunk *chunk, OpCode op) {
    size_t count = 0;

    for (size_t i = 0; i < chunk->instructions.size; i++) {
        if (VM_DECODE_OPCODE(chunk->instructions.data[i]) == op) {
            count++;
        }
    }

    return count;
}

// The index of the first instruction with this opcode, or -1. Lets a test say
// "the jump comes before the block it skips" without naming either position.
static inline long test_find_opcode(const Chunk *chunk, OpCode op) {
    for (size_t i = 0; i < chunk->instructions.size; i++) {
        if (VM_DECODE_OPCODE(chunk->instructions.data[i]) == op) {
            return (long)i;
        }
    }

    return -1;
}

static inline Instruction test_instruction(const Chunk *chunk, size_t index) {
    assert(index < chunk->instructions.size);

    return chunk->instructions.data[index];
}

#endif
