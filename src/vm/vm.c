#include "vm/vm.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "vm/codegen.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Chunk *chunk_create() {
    Chunk *chunk = malloc(sizeof(Chunk));
    chunk->instructions = malloc(sizeof(Instruction) * VM_CHUNK_INITIAL_CAPACITY);
    chunk->size = 0;
    chunk->capacity = VM_CHUNK_INITIAL_CAPACITY;
    chunk->const_pool = constpool_create(VM_MAX_CONSTANTS);
    return chunk;
}

size_t chunk_add_instruction(Chunk *chunk, Instruction instruction) {
    if (chunk->size >= chunk->capacity) {
        chunk->capacity *= 2;
        chunk->instructions = realloc(chunk->instructions, chunk->capacity * sizeof(Instruction));
    }

    chunk->instructions[chunk->size++] = instruction;

    return chunk->size - 1;
}

void chunk_patch_instruction(Chunk *chunk, size_t index, Instruction instruction) {
    assert(index < chunk->size);

    chunk->instructions[index] = instruction;
}

void chunk_free(Chunk *chunk) {
    constpool_free(chunk->const_pool);

    free(chunk->instructions);
    free(chunk);
}

VM *vm_create() {
    VM *vm = malloc(sizeof(VM));
    vm->instruction_pointer = 0;

    memset(vm->registers, 0, sizeof(vm->registers));
    return vm;
}

float vm_add(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var + vm->registers[r2].number_var;
}

float vm_sub(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var - vm->registers[r2].number_var;
}

float vm_mul(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var * vm->registers[r2].number_var;
}

float vm_div(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var / vm->registers[r2].number_var;
}

void vm_arithmetic(VM *vm, Instruction instruction, float (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm->registers[rd].number_var = func(vm, r1, r2);
}

bool vm_less_than(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var < vm->registers[r2].number_var;
}

bool vm_greater_than(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var > vm->registers[r2].number_var;
}

bool vm_equal(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var == vm->registers[r2].number_var;
}

bool vm_not_equal(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var != vm->registers[r2].number_var;
}

bool vm_less_equal(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var <= vm->registers[r2].number_var;
}

bool vm_greater_equal(VM *vm, size_t r1, size_t r2) {
    return vm->registers[r1].number_var >= vm->registers[r2].number_var;
}

void vm_conditional(VM *vm, Instruction instruction, bool (*func)(VM *, size_t, size_t)) {
    size_t rd = VM_DECODE_R_RD(instruction);
    size_t r1 = VM_DECODE_R_R1(instruction);
    size_t r2 = VM_DECODE_R_R2(instruction);

    vm->registers[rd].bool_var = func(vm, r1, r2);
}

void vm_execute(VM *vm, const char *source) {
    Lexer lexer = lexer_create(source);
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);

    ast_script_resolve_symbols(script);

    Chunk *chunk = codegen_generate(script);

    vm->instruction_pointer = 0;

    bool returned = false;
    while (!returned && vm->instruction_pointer < chunk->size) {
        Instruction instruction = chunk->instructions[vm->instruction_pointer];

        OpCode op = VM_DECODE_OPCODE(instruction);
        switch (op) {
        case OP_LOAD_CONST: {
            size_t reg = VM_DECODE_I_RD(instruction);
            size_t const_index = VM_DECODE_I_KX(instruction);
            vm->registers[reg] = constpool_get(chunk->const_pool, const_index);
            break;
        }
        case OP_MOVE: {
            int rd = VM_DECODE_R_RD(instruction);
            int r1 = VM_DECODE_R_R1(instruction);

            vm->registers[rd] = vm->registers[r1];
            break;
        }
        case OP_ADD: {
            vm_arithmetic(vm, instruction, vm_add);
            break;
        }
        case OP_SUB: {
            vm_arithmetic(vm, instruction, vm_sub);
            break;
        }
        case OP_MUL: {
            vm_arithmetic(vm, instruction, vm_mul);
            break;
        }
        case OP_DIV: {
            vm_arithmetic(vm, instruction, vm_div);
            break;
        }
        case OP_CMP_LT: {
            vm_conditional(vm, instruction, vm_less_than);
            break;
        }
        case OP_CMP_GT: {
            vm_conditional(vm, instruction, vm_greater_than);
            break;
        }
        case OP_CMP_EQ: {
            vm_conditional(vm, instruction, vm_equal);
            break;
        }
        case OP_CMP_NE: {
            vm_conditional(vm, instruction, vm_not_equal);
            break;
        }
        case OP_CMP_LE: {
            vm_conditional(vm, instruction, vm_less_equal);
            break;
        }
        case OP_CMP_GE: {
            vm_conditional(vm, instruction, vm_less_equal);
            break;
        }
        case OP_RETURN: {
            size_t r1 = VM_DECODE_R_R1(instruction);
            vm->result = vm->registers[r1];
            returned = true;
            break;
        }
        case OP_JMP: {
            vm->instruction_pointer += VM_DECODE_I_IMM(instruction);
            break;
        }
        case OP_JMP_IF_FALSE: {
            size_t reg = VM_DECODE_I_RD(instruction);

            bool cond = vm->registers[reg].bool_var;
            if (!cond) {
                size_t offset = VM_DECODE_I_IMM(instruction);
                vm->instruction_pointer += offset;
            }

            break;
        }
        }

        vm->instruction_pointer += 1;
    }

    chunk_free(chunk);
    ast_script_free(script);
}

Variant vm_get_result(VM *vm) { return vm->result; }

void vm_free(VM *vm) { free(vm); }
