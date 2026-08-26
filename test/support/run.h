#ifndef GAB_TEST_RUN_H
#define GAB_TEST_RUN_H

// Running a snippet of source and asking what came back. Every feature test
// wants some version of this, so it lives here rather than being copied: a
// change to what "ran correctly" means -- an extra leak assertion, say --
// should have exactly one place to be made.
//
// Header-only and 'static inline', matching test_context.h. The tests are
// separate executables with no shared translation unit to link against.

#include "ast/resolve.h"
#include "compile.h"
#include "lexer.h"
#include "object.h"
#include "parser.h"
#include "scope.h"
#include "slot.h"
#include "support/test_context.h"
#include "vm/chunk.h"
#include "vm/interp.h"
#include "vm/opcode.h"
#include "vm/vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Runs a script and copies the top-level result out of slot 0, as wide as the
// caller says it is. The width is the caller's because a slot carries no tag:
// only the test knows what type the script it wrote ends on.
//
// The frame-count assertion is the shared correctness check: a script that
// returned normally has unwound every frame, so a non-zero count means the run
// went wrong in a way the returned value alone would not show.
static inline void test_run(const char *source, void *out, size_t width) {
    VM *vm = vm_create();

    compile_and_run(vm, test_in_a_module(source));

    assert(vm->frame_count == 0);

    memcpy(out, vm_slot_at(vm, 0), width);

    vm_free(vm);
}

// Runs a script whose top level ends in a 'string' and copies the characters
// out while the VM still lives. A string header borrows: its characters belong
// to the VM's arena, so reading them after vm_free would be a use-after-free --
// exactly the dangle the language says a borrow can become.
static inline void test_run_string(const char *source, char *out, size_t capacity, int32_t *out_length) {
    VM *vm = vm_create();

    compile_and_run(vm, test_in_a_module(source));

    assert(vm->frame_count == 0);

    GabStringValue value;
    memcpy(&value, vm_slot_at(vm, 0), sizeof(value));

    assert((size_t)value.length < capacity);

    memcpy(out, value.data, (size_t)value.length);
    out[value.length] = '\0';
    *out_length = value.length;

    vm_free(vm);
}
static inline int test_run_int(const char *source) {
    int32_t result;
    test_run(source, &result, sizeof(result));

    return result;
}

static inline float test_run_float(const char *source) {
    float result;
    test_run(source, &result, sizeof(result));

    return result;
}

static inline bool test_run_bool(const char *source) {
    int32_t result;
    test_run(source, &result, sizeof(result));

    return result != 0;
}

// Compiles as far as resolution and leaves the results in the caller's scope
// and script, so a test can inspect the symbols and types the front end
// settled on. The caller owns both, since what is worth inspecting differs.
static inline bool test_resolve(TestContext *ctx, Scope *scope, ASTUnit *unit, const char *source) {
    Lexer lexer = lexer_create(test_in_a_module(source), ctx->arena, &ctx->strings, &ctx->diagnostics);
    Parser parser = parser_create(&lexer, &ctx->diagnostics);

    if (!parser_parse(&parser, unit)) {
        return false;
    }

    return resolve_unit(ctx->arena, unit, scope, NULL, &ctx->diagnostics);
}

// Whether a source compiles at all, for the cases that only care that a bad
// program is rejected. Owns its own context, since nothing is inspected after.
static inline bool test_compiles(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    bool ok = test_resolve(&ctx, scope, unit, source);

    ast_unit_destroy(unit);
    test_context_free(&ctx);

    return ok;
}

// Whether some diagnostic the resolver reported contains 'needle'. For a rule
// whose message teaches a remedy: that an error was reported says the rule
// holds, and only the text says the programmer was told what to do about it.
static inline bool test_diagnostic_mentions(const char *source, const char *needle) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create();

    test_resolve(&ctx, scope, unit, source);

    bool found = false;

    for (size_t i = 0; i < diagnostics_count(&ctx.diagnostics); i++) {
        if (strstr(diagnostics_get(&ctx.diagnostics, i)->message, needle)) {
            found = true;
            break;
        }
    }

    ast_unit_destroy(unit);
    test_context_free(&ctx);

    return found;
}

// Whether a source survives codegen, which is further than test_compiles goes.
// A rule enforced while emitting code -- an ownership rule, say -- rejects a
// program the resolver was happy with, and only this can see that.
static inline bool test_codegens(const char *source) {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype script;
    bool ok = compile_unit(vm, test_in_a_module(source), &script, &diagnostics);

    diagnostics_free(&diagnostics);

    if (ok) {
        func_proto_free(&script);
    }

    vm_free(vm);

    return ok;
}

// Runs a script expected to fail, and returns why. Compilation must still
// succeed: this is for runtime traps, where the mistake is only visible once
// the offending instruction executes.
static inline VmRunStatus test_run_status(const char *source) {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype script;
    bool compiled = compile_unit(vm, test_in_a_module(source), &script, &diagnostics);

    diagnostics_free(&diagnostics);
    assert(compiled);

    VmRunStatus status = interp_run_top_level(vm, &script);

    // The status is reported both ways round, and a failure always carries a
    // message for the host to show.
    assert(vm->error.status == status);
    assert(status == VM_RUN_OK || vm->error.message[0] != '\0');

    func_proto_free(&script);
    vm_free(vm);

    return status;
}

// Whether a source compiles against a real VM, which is what a program calling
// a builtin method needs: the builtins are registered on the VM's registry, so
// a scope built without one resolves no method at all and would report a
// failure whatever the rule under test says.
static inline bool test_compiles_on_vm(const char *source) {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype script;
    bool compiled = compile_unit(vm, test_in_a_module(source), &script, &diagnostics);

    diagnostics_free(&diagnostics);

    if (compiled) {
        func_proto_free(&script);
    }

    vm_free(vm);

    return compiled;
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
    FuncPrototype script;
} TestProgram;

static inline TestProgram test_compile(const char *source) {
    TestProgram program = {.vm = vm_create()};

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, program.vm->env.compile_arena, "<test>");

    bool ok = compile_unit(program.vm, test_in_a_module(source), &program.script, &diagnostics);

    diagnostics_free(&diagnostics);
    assert(ok);

    return program;
}

// Compiles a further unit into the same VM, replacing the program's script with
// it. For the claims that need two units: an index a second unit encodes means
// nothing unless the first has already taken the ones below it.
static inline void test_compile_next(TestProgram *program, const char *source) {
    func_proto_free(&program->script);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, program->vm->env.compile_arena, "<test>");

    bool ok = compile_unit(program->vm, test_in_a_module(source), &program->script, &diagnostics);

    diagnostics_free(&diagnostics);
    assert(ok);
}

// Where a type sits in the VM's list, which is what OP_NEW encodes. -1 if the
// VM has never been asked to allocate it.
static inline long test_heap_type_index(TestProgram *program, const char *name) {
    for (size_t i = 0; i < program->vm->program.heap_types.size; i++) {
        if (strcmp(program->vm->program.heap_types.data[i]->name->data, name) == 0) {
            return (long)i;
        }
    }

    return -1;
}

static inline void test_program_free(TestProgram *program) {

    func_proto_free(&program->script);
    vm_free(program->vm);
}

static inline Chunk *test_top_chunk(TestProgram *program) { return program->script.chunk; }

// The chunk of a declared function, by declaration order. A builtin method is
// an extern, in its own table, so nothing precedes what a unit contributed.
static inline Chunk *test_func_chunk(TestProgram *program, size_t index) {
    assert(index < program->vm->program.prototypes.size);

    return program->vm->program.prototypes.data[index]->chunk;
}

// The prototype of a declared function, and how many the loaded units
// contributed.
static inline FuncPrototype *test_func_proto(TestProgram *program, size_t index) {
    return program->vm->program.prototypes.data[index];
}

static inline size_t test_func_count(TestProgram *program) { return program->vm->program.prototypes.size; }

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

// The I-type operand of the first instruction with this opcode, or -1.
static inline long test_first_operand(const Chunk *chunk, OpCode op) {
    long at = test_find_opcode(chunk, op);

    if (at < 0) {
        return -1;
    }

    return (long)VM_DECODE_I_KX(chunk->instructions.data[at]);
}

static inline Instruction test_instruction(const Chunk *chunk, size_t index) {
    assert(index < chunk->instructions.size);

    return chunk->instructions.data[index];
}

#endif
