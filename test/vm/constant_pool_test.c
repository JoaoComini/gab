#include "value.h"
#include "vm/constant_pool.h"
#include <assert.h>

static void test_create_and_free() {
    ConstantPool *pool = constpool_create(100);
    assert(pool->constants != NULL);
    assert(pool->count == 0);
    assert(pool->capacity == 4);
    assert(pool->max_capacity == 100);

    constpool_free(pool);
}

static void test_basic_add_and_get() {
    ConstantPool *pool = constpool_create(10);
    Value v1 = {.type = TYPE_FLOAT, .as_float = 3.14f};
    Value v2 = {.type = TYPE_INT, .as_int = 2};

    int index1 = constpool_add(pool, v1);
    assert(index1 == 0);
    assert(pool->count == 1);

    int index2 = constpool_add(pool, v2);
    assert(index2 == 1);
    assert(pool->count == 2);

    Value retrieved = constpool_get(pool, 0);
    assert(retrieved.type == TYPE_FLOAT);
    assert(retrieved.as_float == 3.14f);

    retrieved = constpool_get(pool, 1);
    assert(retrieved.type == TYPE_INT);
    assert(retrieved.as_int == 2);

    constpool_free(pool);
}

static void test_auto_resize() {
    ConstantPool *pool = constpool_create(100);

    // Fill initial capacity
    Value v = {.type = TYPE_FLOAT, .as_float = 1.0f};
    for (size_t i = 0; i < 4; i++) {
        constpool_add(pool, v);
    }
    assert(pool->capacity == 4);

    // Trigger resize
    constpool_add(pool, v);
    assert(pool->capacity == 8);
    assert(pool->count == 5);

    constpool_free(pool);
}

int main(void) {
    test_create_and_free();
    test_basic_add_and_get();
    test_auto_resize();

    return 0;
}
