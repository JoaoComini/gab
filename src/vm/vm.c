#include "vm/vm.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "vm/codegen.h"
#include "vm/constant_pool.h"
#include "vm/opcode.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Chunk *chunk_create() {
    Chunk *chunk = malloc(sizeof(Chunk));
    chunk->instructions = malloc(sizeof(Instruction) * VM_CHUNK_INITIAL_CAPACITY);
    chunk->instructions_size = 0;
    chunk->instructions_capacity = VM_CHUNK_INITIAL_CAPACITY;
    chunk->const_pool = constpool_create(VM_MAX_CONSTANTS);
    return chunk;
}

void chunk_add_instruction(Chunk *chunk, Instruction instruction) {
    if (chunk->instructions_size >= chunk->instructions_capacity) {
        chunk->instructions_capacity *= 2;
        chunk->instructions =
            realloc(chunk->instructions, chunk->instructions_capacity * sizeof(Instruction));
    }

    chunk->instructions[chunk->instructions_size++] = instruction;
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

void vm_execute(VM *vm, const char *source) {
    Lexer lexer = lexer_create(source);
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);

    Chunk *chunk = codegen_generate(script);

    vm->instruction_pointer = 0;

    bool returned = false;
    while (!returned && vm->instruction_pointer < chunk->instructions_size) {
        Instruction instruction = chunk->instructions[vm->instruction_pointer];
        vm->instruction_pointer += 1;

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
            size_t rd = VM_DECODE_R_RD(instruction);
            size_t r1 = VM_DECODE_R_R1(instruction);
            size_t r2 = VM_DECODE_R_R2(instruction);

            vm->registers[rd].number = vm->registers[r1].number + vm->registers[r2].number;
            break;
        }
        case OP_SUB: {
            size_t rd = VM_DECODE_R_RD(instruction);
            size_t r1 = VM_DECODE_R_R1(instruction);
            size_t r2 = VM_DECODE_R_R2(instruction);
            vm->registers[rd].number = vm->registers[r1].number - vm->registers[r2].number;
            break;
        }
        case OP_MUL: {
            size_t rd = VM_DECODE_R_RD(instruction);
            size_t r1 = VM_DECODE_R_R1(instruction);
            size_t r2 = VM_DECODE_R_R2(instruction);
            vm->registers[rd].number = vm->registers[r1].number * vm->registers[r2].number;
            break;
        }
        case OP_DIV: {
            size_t rd = VM_DECODE_R_RD(instruction);
            size_t r1 = VM_DECODE_R_R1(instruction);
            size_t r2 = VM_DECODE_R_R2(instruction);
            vm->registers[rd].number = vm->registers[r1].number / vm->registers[r2].number;
            break;
        }
        case OP_RETURN: {
            size_t r1 = VM_DECODE_R_R1(instruction);
            returned = true;
        }
        }
    }

    chunk_free(chunk);
    ast_script_free(script);
}

void vm_free(VM *vm) { free(vm); }
