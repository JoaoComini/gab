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

    pool->constants = realloc(pool->constants, pool->capacity * sizeof(SlotWord));
}

ConstantPool *constpool_create(size_t max_capacity) {
    ConstantPool *pool = malloc(sizeof(ConstantPool));
    pool->constants = malloc(CONSTPOLL_INITIAL_CAPACITY * sizeof(SlotWord));
    pool->count = 0;
    pool->capacity = CONSTPOLL_INITIAL_CAPACITY;
    pool->max_capacity = max_capacity;

    return pool;
}

void constpool_free(ConstantPool *pool) {
    free(pool->constants);
    free(pool);
}

/* An instruction names a constant by index, so equal bits need only one entry. A chunk holds a
 * handful, which a scan reaches sooner than a map would hash. */
size_t constpool_add(ConstantPool *pool, SlotWord value) {
    for (size_t i = 0; i < pool->count; i++) {
        if (memcmp(&pool->constants[i], &value, sizeof(SlotWord)) == 0) {
            return i;
        }
    }

    assert(pool->count < pool->max_capacity);

    if (pool->count == pool->capacity) {
        constpool_resize(pool);
    }

    pool->constants[pool->count] = value;
    return pool->count++;
}

SlotWord constpool_get(const ConstantPool *pool, size_t index) {
    assert(index < pool->count);

    return pool->constants[index];
}
