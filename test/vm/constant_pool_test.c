#include "vm/constant_pool.h"
#include "vm/slot.h"
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
    SlotWord v1 = {.as_float = 3.14f};
    SlotWord v2 = {.as_int = 2};

    int index1 = constpool_add(pool, v1);
    assert(index1 == 0);
    assert(pool->count == 1);

    int index2 = constpool_add(pool, v2);
    assert(index2 == 1);
    assert(pool->count == 2);

    SlotWord retrieved = constpool_get(pool, 0);
    assert(retrieved.as_float == 3.14f);

    retrieved = constpool_get(pool, 1);
    assert(retrieved.as_int == 2);

    constpool_free(pool);
}

/* Two writes of one value share an entry, since an index is all an instruction names. */
static void test_an_equal_value_is_held_once() {
    ConstantPool *pool = constpool_create(10);

    size_t first = constpool_add(pool, (SlotWord){.as_int = 7});
    size_t again = constpool_add(pool, (SlotWord){.as_int = 7});

    assert(first == again);
    assert(pool->count == 1);

    size_t other = constpool_add(pool, (SlotWord){.as_int = 8});

    assert(other != first);
    assert(pool->count == 2);

    constpool_free(pool);
}

/* A float and an int sharing a bit pattern are not the same constant, since one slot holds either. */
static void test_a_value_of_another_type_is_its_own() {
    ConstantPool *pool = constpool_create(10);

    SlotWord zero_int = {.as_int = 0};
    SlotWord zero_float = {.as_float = 0.0f};

    size_t first = constpool_add(pool, zero_int);
    size_t second = constpool_add(pool, zero_float);

    /* Both are all-zero bits, so one entry serves them; what an instruction reads it as is its own. */
    assert(first == second);
    assert(constpool_get(pool, first).as_int == 0);

    constpool_free(pool);
}

static void test_auto_resize() {
    ConstantPool *pool = constpool_create(100);

    for (size_t i = 0; i < 4; i++) {
        constpool_add(pool, (SlotWord){.as_int = (int32_t)i});
    }
    assert(pool->capacity == 4);

    constpool_add(pool, (SlotWord){.as_int = 4});
    assert(pool->capacity == 8);
    assert(pool->count == 5);

    constpool_free(pool);
}

int main(void) {
    test_create_and_free();
    test_basic_add_and_get();
    test_an_equal_value_is_held_once();
    test_a_value_of_another_type_is_its_own();
    test_auto_resize();

    return 0;
}
