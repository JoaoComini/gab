#ifndef GAB_CONSTPOOL_H
#define GAB_CONSTPOOL_H

#include "value.h"
#include <stddef.h>

typedef struct {
    Value *constants;
    size_t count;
    size_t capacity;
    size_t max_capacity;
} ConstantPool;

ConstantPool *constpool_create(size_t max_capacity);
void constpool_free(ConstantPool *pool);
size_t constpool_add(ConstantPool *pool, Value value);
Value constpool_get(const ConstantPool *pool, size_t index);

#endif
