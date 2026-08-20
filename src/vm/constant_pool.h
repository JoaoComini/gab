#ifndef GAB_CONSTPOOL_H
#define GAB_CONSTPOOL_H

#include "slot.h"
#include <stddef.h>
#include <stdint.h>

// A constant as the pool holds it: one slot wide, and either an int or a float
// depending on the literal it came from. Untagged like a slot, since the
// instruction that loads it already knows which of the two it is.
//
// This is a pool entry rather than a stack slot, which is why it is a type at
// all: the pool is a homogeneous array indexed by number, so the value is the
// unit here, where on the stack the unit is a run of bytes.
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
