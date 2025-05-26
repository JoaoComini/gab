#include "util/hash_map.h"

#include <assert.h>
#include <stdio.h>

#define test_map_hash(key) (size_t)key
#define test_map_key_equals(key, other) key == other
#define test_map_key_dup(key) key
#define test_map_entry_free(key, value)

GAB_HASH_MAP(TestMap, test_map, int, int)

static void test_create_and_free() {
    TestMap *table = test_map_create(8);
    assert(table != NULL);
    assert(table->capacity == 8);
    assert(table->size == 0);
    test_map_destroy(table);
}

static void test_basic_insert_and_lookup() {
    TestMap *table = test_map_create(8);

    assert(test_map_insert(table, 0, 0));
    assert(table->size == 1);
    assert(test_map_insert(table, 1, 1));
    assert(table->size == 2);

    int *x = test_map_lookup(table, 0);
    assert(x != NULL);
    assert(*x == 0);

    int *y = test_map_lookup(table, 1);
    assert(y != NULL);
    assert(*y == 1);

    assert(test_map_lookup(table, 2) == NULL);

    test_map_destroy(table);
}

static void test_duplicate_insert() {
    TestMap *table = test_map_create(8);

    assert(test_map_insert(table, 0, 0));
    assert(table->size == 1);

    assert(test_map_insert(table, 0, 0) == NULL);
    assert(table->size == 1); // Size shouldn't change

    int *x = test_map_lookup(table, 0);
    assert(*x == 0);

    test_map_destroy(table);
}

static void test_delete() {
    TestMap *table = test_map_create(8);

    assert(test_map_insert(table, 0, 0));
    assert(test_map_insert(table, 1, 1));
    assert(table->size == 2);

    test_map_delete(table, 0);
    assert(table->size == 1);
    assert(test_map_lookup(table, 0) == NULL);
    assert(test_map_lookup(table, 1) != NULL);

    test_map_delete(table, 2);
    assert(table->size == 1);

    test_map_destroy(table);
}

static void test_collision_handling() {
    TestMap *table = test_map_create(2);

    assert(test_map_insert(table, 0, 0));
    assert(test_map_insert(table, 1, 1));
    assert(test_map_insert(table, 2, 2));
    assert(table->size == 3);

    assert(test_map_lookup(table, 0) != NULL);
    assert(test_map_lookup(table, 1) != NULL);
    assert(test_map_lookup(table, 2) != NULL);

    test_map_destroy(table);
}

static void test_resize() {
    TestMap *table = test_map_create(4);

    assert(table->capacity == 4);
    assert(test_map_insert(table, 0, 0));
    assert(test_map_insert(table, 1, 1));
    assert(test_map_insert(table, 2, 2));

    assert(table->capacity == 8);
    assert(table->size == 3);

    assert(test_map_lookup(table, 0) != NULL);
    assert(test_map_lookup(table, 1) != NULL);
    assert(test_map_lookup(table, 2) != NULL);

    test_map_destroy(table);
}

static void test_delete_all() {
    TestMap *table = test_map_create(8);

    int keys[] = {0, 1, 2, 3};
    for (int i = 0; i < 4; i++) {
        assert(test_map_insert(table, keys[i], i));
    }
    assert(table->size == 4);

    for (int i = 0; i < 4; i++) {
        test_map_delete(table, keys[i]);
    }
    assert(table->size == 0);

    for (int i = 0; i < 4; i++) {
        assert(test_map_lookup(table, keys[i]) == NULL);
    }

    test_map_destroy(table);
}

static void test_stress() {
    TestMap *table = test_map_create(16);
    const int NUM_ENTRIES = 1000;

    for (int i = 0; i < NUM_ENTRIES; i++) {
        assert(test_map_insert(table, i, i));
    }
    assert(table->size == NUM_ENTRIES);

    for (int i = 0; i < NUM_ENTRIES; i++) {
        int *entry = test_map_lookup(table, i);
        assert(entry != NULL);
        assert(*entry == i);
    }

    for (int i = 0; i < NUM_ENTRIES; i += 2) {
        test_map_delete(table, i);
    }

    assert(table->size == NUM_ENTRIES / 2);

    test_map_destroy(table);
}

int main() {
    test_create_and_free();
    test_basic_insert_and_lookup();
    test_duplicate_insert();
    test_delete();
    test_collision_handling();
    test_resize();
    test_delete_all();
    test_stress();

    return 0;
}
