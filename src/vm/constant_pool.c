#include "constant_pool.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define CONSTPOLL_INITIAL_CAPACITY 4

static void constpool_resize(ConstantPool *pool) {
    pool->capacity *= 2;

    if (pool->capacity > pool->max_capacity) {
        pool->capacity = pool->max_capacity;
    }

    pool->constants = realloc(pool->constants, pool->capacity * sizeof(Value));
}

ConstantPool *constpool_create(size_t max_capacity) {
    ConstantPool *pool = malloc(sizeof(ConstantPool));
    pool->constants = malloc(CONSTPOLL_INITIAL_CAPACITY * sizeof(Value));
    pool->count = 0;
    pool->capacity = CONSTPOLL_INITIAL_CAPACITY;
    pool->max_capacity = max_capacity;

    return pool;
}

void constpool_free(ConstantPool *pool) {
    free(pool->constants);
    free(pool);
}

size_t constpool_add(ConstantPool *pool, Value value) {
    assert(pool->count < pool->max_capacity);

    if (pool->count == pool->capacity) {
        constpool_resize(pool);
    }

    pool->constants[pool->count] = value;
    return pool->count++;
}

Value constpool_get(const ConstantPool *pool, size_t index) {
    assert(index < pool->count);

    return pool->constants[index];
}
