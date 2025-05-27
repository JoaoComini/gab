#ifndef GAB_CHUNK_H
#define GAB_CHUNK_H

#include "util/list.h"
#include "vm/constant_pool.h"

#include <stdint.h>

typedef uint32_t Instruction;

#define instruction_list_item_free(item)
GAB_LIST(InstructionList, instruction_list, Instruction)

typedef struct {
    ConstantPool *const_pool;
    InstructionList instructions;
} Chunk;

Chunk *chunk_create();
size_t chunk_add_instruction(Chunk *chunk, Instruction instruction);
void chunk_patch_instruction(Chunk *chunk, size_t index, Instruction instruction);
void chunk_free(Chunk *chunk);

#endif
