#include "vm/vm.h"

#include "arena.h"
#include "ast/ast.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "type.h"
#include "vm/chunk.h"
#include "vm/codegen.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BLOCK_SIZE 2048
#define VM_INITIAL_STACK_SIZE 256

VM *vm_create() {
    VM *vm = malloc(sizeof(VM));
    vm->instruction_pointer = 0;

    vm->global_data = value_list_create();
    vm->global_funcs = func_proto_list_create();

    vm->persistent_arena = arena_create(ARENA_BLOCK_SIZE);
    vm->transient_arena = arena_create(ARENA_BLOCK_SIZE);

    // The pool must be live before the global scope: scope_init builds the
    // TypeRegistry, which interns the builtin type names.
    string_pool_init(&vm->strings, vm->persistent_arena);

    scope_init(&vm->global_scope, vm->persistent_arena, &vm->strings, NULL);

    vm->stack_capacity = VM_INITIAL_STACK_SIZE;
    vm->stack = calloc(vm->stack_capacity, sizeof(Value));
    vm->registers = vm->stack;
    vm->frame_count = 0;

    return vm;
}

// Registers are stack[base + r], so the stack must hold every register the
// frame can address before it starts executing.
static bool vm_reserve_stack(VM *vm, size_t needed) {
    if (needed <= vm->stack_capacity) {
        return true;
    }

    size_t capacity = vm->stack_capacity;
    while (capacity < needed) {
        capacity *= 2;
    }

    Value *stack = realloc(vm->stack, capacity * sizeof(Value));
    if (!stack) {
        return false;
    }

    memset(stack + vm->stack_capacity, 0, (capacity - vm->stack_capacity) * sizeof(Value));

    // realloc may move the buffer, so anything pointing into it is rebased.
    size_t offset = vm->frame_count > 0 ? vm->frames[vm->frame_count - 1].base : 0;

    vm->stack = stack;
    vm->stack_capacity = capacity;
    vm->registers = stack + offset;

    return true;
}

static bool vm_push_frame(VM *vm, const FuncPrototype *proto, size_t base, size_t return_ip,
                          unsigned int dest) {
    if (vm->frame_count == VM_MAX_CALL_DEPTH) {
        return false;
    }

    if (!vm_reserve_stack(vm, base + proto->max_registers)) {
        return false;
    }

    vm->frames[vm->frame_count++] = (CallFrame){
        .proto = proto,
        .return_ip = return_ip,
        .base = base,
        .dest = dest,
    };

    vm->registers = vm->stack + base;
    vm->instruction_pointer = 0;

    return true;
}

static void vm_pop_frame(VM *vm) {
    CallFrame frame = vm->frames[--vm->frame_count];

    if (vm->frame_count == 0) {
        vm->registers = vm->stack;
        return;
    }

    vm->registers = vm->stack + vm->frames[vm->frame_count - 1].base;
    vm->instruction_pointer = frame.return_ip;
}

void vm_free(VM *vm) {
    value_list_free(&vm->global_data);
    func_proto_list_free(&vm->global_funcs);

    // Frees the bucket array, which walks entries — must happen before the
    // arena holding the string payloads is destroyed.
    string_pool_free(&vm->strings);

    free(vm->stack);

    arena_destroy(vm->persistent_arena);
    arena_destroy(vm->transient_arena);

    free(vm);
}

float vm_addf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float + vm->registers[r2].as_float;
}

float vm_subf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float - vm->registers[r2].as_float;
}

float vm_mulf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float * vm->registers[r2].as_float;
}

float vm_divf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float / vm->registers[r2].as_float;
}

void vm_arithmeticf(VM *vm, Instruction instruction, float (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm->registers[rd].as_float = func(vm, r1, r2);
}

int vm_addi(VM *vm, size_t r1, size_t r2) { return vm->registers[r1].as_int + vm->registers[r2].as_int; }

int vm_subi(VM *vm, size_t r1, size_t r2) { return vm->registers[r1].as_int - vm->registers[r2].as_int; }

int vm_muli(VM *vm, size_t r1, size_t r2) { return vm->registers[r1].as_int * vm->registers[r2].as_int; }

int vm_divi(VM *vm, size_t r1, size_t r2) { return vm->registers[r1].as_int / vm->registers[r2].as_int; }

void vm_arithmetici(VM *vm, Instruction instruction, int (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm->registers[rd].as_int = func(vm, r1, r2);
}

bool vm_less_thanf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float < vm->registers[r2].as_float;
}

bool vm_greater_thanf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float > vm->registers[r2].as_float;
}

bool vm_equalf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float == vm->registers[r2].as_float;
}

bool vm_not_equalf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float != vm->registers[r2].as_float;
}

bool vm_less_equalf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float <= vm->registers[r2].as_float;
}

bool vm_greater_equalf(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_float >= vm->registers[r2].as_float;
}

bool vm_less_thani(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_int < vm->registers[r2].as_int;
}

bool vm_greater_thani(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_int > vm->registers[r2].as_int;
}

bool vm_equali(VM *vm, size_t r1, size_t r2) { return vm->registers[r1].as_int == vm->registers[r2].as_int; }

bool vm_not_equali(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_int != vm->registers[r2].as_int;
}

bool vm_less_equali(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_int <= vm->registers[r2].as_int;
}

bool vm_greater_equali(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].as_int >= vm->registers[r2].as_int;
}

void vm_conditional(VM *vm, Instruction instruction, bool (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm->registers[rd].type = TYPE_BOOL;
    vm->registers[rd].as_int = func(vm, r1, r2);
}

void vm_execute(VM *vm, const char *source) {
    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->transient_arena, "<script>");

    Lexer lexer = lexer_create(source, &diagnostics);
    Parser parser = parser_create(&lexer, &diagnostics);
    ASTScript *script = ast_script_create();

    // Each stage is a precondition for the next: a failure must stop the
    // pipeline rather than let a malformed AST reach codegen.
    Chunk *chunk = NULL;
    unsigned int max_registers = 0;

    if (parser_parse(&parser, script) &&
        ast_script_resolve(vm->transient_arena, script, &vm->global_scope, &diagnostics)) {
        chunk = codegen_generate(script, &vm->global_data, &vm->global_funcs, &diagnostics, &max_registers);
    }

    if (!chunk) {
        diagnostics_print(&diagnostics, stderr);

        diagnostics_free(&diagnostics);
        ast_script_destroy(script);
        return;
    }

    diagnostics_free(&diagnostics);

    // The top level runs as frame zero, so the interpreter has a single path
    // and OP_RETURN means the same thing everywhere.
    FuncPrototype top_level = {
        .chunk = chunk,
        .arity = 0,
        .max_registers = (int)max_registers,
    };

    vm->frame_count = 0;
    if (!vm_push_frame(vm, &top_level, 0, 0, 0)) {
        fprintf(stderr, "<script>: out of stack space\n");

        chunk_free(chunk);
        ast_script_destroy(script);
        return;
    }

    while (vm->frame_count > 0 &&
           vm->instruction_pointer < vm->frames[vm->frame_count - 1].proto->chunk->instructions.size) {
        CallFrame *frame = &vm->frames[vm->frame_count - 1];
        Chunk *chunk = frame->proto->chunk;

        Instruction instruction = instruction_list_get(&chunk->instructions, vm->instruction_pointer);

        OpCode op = VM_DECODE_OPCODE(instruction);
        switch (op) {
        case OP_LOAD_CONST: {
            size_t reg = VM_DECODE_I_RD(instruction);
            size_t const_index = VM_DECODE_I_KX(instruction);
            vm->registers[reg] = constpool_get(chunk->const_pool, const_index);
            break;
        }
        case OP_LOAD_TRUE: {
            size_t reg = VM_DECODE_I_RD(instruction);
            vm->registers[reg].as_int = 1;
            break;
        }
        case OP_LOAD_FALSE: {
            size_t reg = VM_DECODE_I_RD(instruction);
            vm->registers[reg].as_int = 0;
            break;
        }
        case OP_MOVE: {
            int rd = VM_DECODE_R_RD(instruction);
            int r1 = VM_DECODE_R_R1(instruction);

            vm->registers[rd] = vm->registers[r1];
            break;
        }
        case OP_ADDF: {
            vm_arithmeticf(vm, instruction, vm_addf);
            break;
        }
        case OP_SUBF: {
            vm_arithmeticf(vm, instruction, vm_subf);
            break;
        }
        case OP_MULF: {
            vm_arithmeticf(vm, instruction, vm_mulf);
            break;
        }
        case OP_DIVF: {
            vm_arithmeticf(vm, instruction, vm_divf);
            break;
        }
        case OP_CMP_LTF: {
            vm_conditional(vm, instruction, vm_less_thanf);
            break;
        }
        case OP_CMP_GTF: {
            vm_conditional(vm, instruction, vm_greater_thanf);
            break;
        }
        case OP_CMP_EQF: {
            vm_conditional(vm, instruction, vm_equalf);
            break;
        }
        case OP_CMP_NEF: {
            vm_conditional(vm, instruction, vm_not_equalf);
            break;
        }
        case OP_CMP_LEF: {
            vm_conditional(vm, instruction, vm_less_equalf);
            break;
        }
        case OP_CMP_GEF: {
            vm_conditional(vm, instruction, vm_less_equalf);
            break;
        }
        case OP_ADDI: {
            vm_arithmetici(vm, instruction, vm_addi);
            break;
        }
        case OP_SUBI: {
            vm_arithmetici(vm, instruction, vm_subi);
            break;
        }
        case OP_MULI: {
            vm_arithmetici(vm, instruction, vm_muli);
            break;
        }
        case OP_DIVI: {
            vm_arithmetici(vm, instruction, vm_divi);
            break;
        }
        case OP_CMP_LTI: {
            vm_conditional(vm, instruction, vm_less_thani);
            break;
        }
        case OP_CMP_GTI: {
            vm_conditional(vm, instruction, vm_greater_thani);
            break;
        }
        case OP_CMP_EQI: {
            vm_conditional(vm, instruction, vm_equali);
            break;
        }
        case OP_CMP_NEI: {
            vm_conditional(vm, instruction, vm_not_equali);
            break;
        }
        case OP_CMP_LEI: {
            vm_conditional(vm, instruction, vm_less_equali);
            break;
        }
        case OP_CMP_GEI: {
            vm_conditional(vm, instruction, vm_less_equali);
            break;
        }
        case OP_RETURN: {
            size_t r1 = VM_DECODE_R_R1(instruction);
            Value result = vm->registers[r1];

            unsigned int dest = frame->dest;
            vm_pop_frame(vm);

            if (vm->frame_count == 0) {
                // Frame zero returning ends execution; r0 keeps the result so
                // callers can still read it.
                vm->stack[0] = result;
                continue;
            }

            vm->registers[dest] = result;
            continue;
        }
        case OP_JMP: {
            vm->instruction_pointer += VM_DECODE_I_IMM(instruction);
            break;
        }
        case OP_JMP_IF_FALSE: {
            size_t reg = VM_DECODE_I_RD(instruction);

            bool cond = vm->registers[reg].as_int;
            if (!cond) {
                size_t offset = VM_DECODE_I_IMM(instruction);
                vm->instruction_pointer += offset;
            }

            break;
        }
        case OP_JMP_IF_TRUE: {
            size_t reg = VM_DECODE_I_RD(instruction);

            bool cond = vm->registers[reg].as_int;
            if (cond) {
                size_t offset = VM_DECODE_I_IMM(instruction);
                vm->instruction_pointer += offset;
            }

            break;
        }
        case OP_LOAD_GLOBAL: {
            size_t rd = VM_DECODE_I_RD(instruction);
            size_t index = VM_DECODE_I_IMM(instruction);

            vm->registers[rd] = value_list_get(&vm->global_data, index);
            break;
        }
        case OP_STORE_GLOBAL: {
            size_t rd = VM_DECODE_I_RD(instruction);
            size_t index = VM_DECODE_I_IMM(instruction);

            value_list_emplace(&vm->global_data, index, vm->registers[rd]);
            break;
        }
        }

        vm->instruction_pointer += 1;
    }

    // Top-level code has no trailing return, so the loop usually ends by
    // running off the end of the chunk rather than through OP_RETURN.
    while (vm->frame_count > 0) {
        vm_pop_frame(vm);
    }

    arena_reset(vm->transient_arena);
    chunk_free(chunk);
    ast_script_destroy(script);
}
