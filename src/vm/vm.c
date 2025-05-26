#include "vm/vm.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "type.h"
#include "string/string.h"
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

    string_init();

    return vm;
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
    Lexer lexer = lexer_create(source);
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);

    ast_script_resolve(script);

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
            break;
        }
        }

        vm->instruction_pointer += 1;
    }

    chunk_free(chunk);
    ast_script_free(script);
}

Value vm_get_result(VM *vm) { return vm->result; }

void vm_free(VM *vm) {
    string_deinit();
    free(vm);
}
