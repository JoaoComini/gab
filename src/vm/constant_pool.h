#ifndef GAB_CONSTPOOL_H
#define GAB_CONSTPOOL_H

#include "vm/slot.h"
#include <stddef.h>
#include <stdint.h>

typedef union {
    int32_t as_int;
    float as_float;
} SlotWord;

_Static_assert(sizeof(SlotWord) == VM_SLOT_SIZE, "a constant must fill exactly one slot");

typedef struct {
    SlotWord *constants;
    size_t count;
    size_t capacity;
    size_t max_capacity;
} ConstantPool;

ConstantPool *constpool_create(size_t max_capacity);
void constpool_free(ConstantPool *pool);
size_t constpool_add(ConstantPool *pool, SlotWord value);
SlotWord constpool_get(const ConstantPool *pool, size_t index);

#endif
