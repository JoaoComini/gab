#ifndef GAB_TEST_RUN_H
#define GAB_TEST_RUN_H

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

static inline void test_run(const char *source, void *out, size_t width) {
    VM *vm = vm_create();

    compile_and_run(vm, test_in_a_module(source));

    assert(vm->frame_count == 0);

    memcpy(out, vm_slot_at(vm, 0), width);

    vm_free(vm);
}

static inline void test_run_string(const char *source, char *out, size_t capacity, int32_t *out_length) {
    VM *vm = vm_create();

    compile_and_run(vm, test_in_a_module(source));

    assert(vm->frame_count == 0);

    GabStrRef value;
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

static inline bool test_resolve(TestContext *ctx, Scope *scope, ASTUnit *unit, const char *source) {
    Lexer lexer = lexer_create(test_in_a_module(source), ctx->arena, &ctx->strings, &ctx->diagnostics);
    Parser parser = parser_create(&lexer, &ctx->diagnostics);

    if (!parser_parse(&parser, unit)) {
        return false;
    }

    return resolve_unit(ctx->arena, unit, scope, NULL, &ctx->diagnostics);
}

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

static inline VmRunStatus test_run_status(const char *source) {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype script;
    bool compiled = compile_unit(vm, test_in_a_module(source), &script, &diagnostics);

    diagnostics_free(&diagnostics);
    assert(compiled);

    VmRunStatus status = interp_run_top_level(vm, &script);

    assert(vm->error.status == status);
    assert(status == VM_RUN_OK || vm->error.message[0] != '\0');

    func_proto_free(&script);
    vm_free(vm);

    return status;
}

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

static inline void test_compile_next(TestProgram *program, const char *source) {
    func_proto_free(&program->script);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, program->vm->env.compile_arena, "<test>");

    bool ok = compile_unit(program->vm, test_in_a_module(source), &program->script, &diagnostics);

    diagnostics_free(&diagnostics);
    assert(ok);
}

static inline long test_heap_type_index(TestProgram *program, const char *name) {
    for (size_t i = 0; i < program->vm->program.shape_types.size; i++) {
        const String *type_name = type_name_of(program->vm->program.shape_types.data[i]);

        if (type_name && strcmp(type_name->data, name) == 0) {
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

static inline Chunk *test_func_chunk(TestProgram *program, size_t index) {
    assert(index < program->vm->program.prototypes.size);

    return program->vm->program.prototypes.data[index]->chunk;
}

static inline FuncPrototype *test_func_proto(TestProgram *program, size_t index) {
    return program->vm->program.prototypes.data[index];
}

static inline size_t test_func_count(TestProgram *program) { return program->vm->program.prototypes.size; }

static inline size_t test_count_opcode(const Chunk *chunk, OpCode op) {
    size_t count = 0;

    for (size_t i = 0; i < chunk->instructions.size; i++) {
        if (VM_DECODE_OPCODE(chunk->instructions.data[i]) == op) {
            count++;
        }
    }

    return count;
}

static inline long test_find_opcode(const Chunk *chunk, OpCode op) {
    for (size_t i = 0; i < chunk->instructions.size; i++) {
        if (VM_DECODE_OPCODE(chunk->instructions.data[i]) == op) {
            return (long)i;
        }
    }

    return -1;
}

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
