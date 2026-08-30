#ifndef GAB_CONSTPOOL_H
#define GAB_CONSTPOOL_H

#include "slot.h"
#include <stddef.h>
#include <stdint.h>

typedef union {
    int32_t as_int;
    float as_float;
} Constant;

_Static_assert(sizeof(Constant) == VM_SLOT_SIZE, "a constant must fill exactly one slot");

typedef struct {
    Constant *constants;
    size_t count;
    size_t capacity;
    size_t max_capacity;
} ConstantPool;

ConstantPool *constpool_create(size_t max_capacity);
void constpool_free(ConstantPool *pool);
size_t constpool_add(ConstantPool *pool, Constant value);
Constant constpool_get(const ConstantPool *pool, size_t index);

#endif
