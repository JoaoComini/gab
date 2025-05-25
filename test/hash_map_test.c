#include "hash_map.h"
#include "string_ref.h"

#include <assert.h>
#include <stdio.h>

GAB_HASH_MAP(TestMap, test_map, int)

static void test_create_and_free() {
    TestMap *table = test_map_create(8);
    assert(table != NULL);
    assert(table->capacity == 8);
    assert(table->size == 0);
    test_map_free(table);
}

static void test_basic_insert_and_lookup() {
    TestMap *table = test_map_create(8);

    assert(test_map_insert(table, string_ref_create("x"), 0) == true);
    assert(table->size == 1);
    assert(test_map_insert(table, string_ref_create("y"), 1) == true);
    assert(table->size == 2);

    int *x = test_map_lookup(table, string_ref_create("x"));
    assert(x != NULL);
    assert(*x == 0);

    int *y = test_map_lookup(table, string_ref_create("y"));
    assert(y != NULL);
    assert(*y == 1);

    assert(test_map_lookup(table, string_ref_create("z")) == NULL);

    test_map_free(table);
}

static void test_duplicate_insert() {
    TestMap *table = test_map_create(8);

    assert(test_map_insert(table, string_ref_create("x"), 0) == true);
    assert(table->size == 1);

    assert(test_map_insert(table, string_ref_create("x"), 0) == false);
    assert(table->size == 1); // Size shouldn't change

    int *x = test_map_lookup(table, string_ref_create("x"));
    assert(*x == 0);

    test_map_free(table);
}

static void test_delete() {
    TestMap *table = test_map_create(8);

    assert(test_map_insert(table, string_ref_create("x"), 0) == true);
    assert(test_map_insert(table, string_ref_create("y"), 1) == true);
    assert(table->size == 2);

    test_map_delete(table, string_ref_create("x"));
    assert(table->size == 1);
    assert(test_map_lookup(table, string_ref_create("x")) == NULL);
    assert(test_map_lookup(table, string_ref_create("y")) != NULL);

    test_map_delete(table, string_ref_create("z"));
    assert(table->size == 1);

    test_map_free(table);
}

static void test_collision_handling() {
    TestMap *table = test_map_create(2);

    assert(test_map_insert(table, string_ref_create("a"), 0) == true);
    assert(test_map_insert(table, string_ref_create("b"), 1) == true);
    assert(test_map_insert(table, string_ref_create("c"), 2) == true);
    assert(table->size == 3);

    assert(test_map_lookup(table, string_ref_create("a")) != NULL);
    assert(test_map_lookup(table, string_ref_create("b")) != NULL);
    assert(test_map_lookup(table, string_ref_create("c")) != NULL);

    test_map_free(table);
}

static void test_resize() {
    TestMap *table = test_map_create(4);

    assert(table->capacity == 4);
    assert(test_map_insert(table, string_ref_create("a"), 0) == true);
    assert(test_map_insert(table, string_ref_create("b"), 1) == true);
    assert(test_map_insert(table, string_ref_create("c"), 2) == true);

    assert(table->capacity == 8);
    assert(table->size == 3);

    assert(test_map_lookup(table, string_ref_create("a")) != NULL);
    assert(test_map_lookup(table, string_ref_create("b")) != NULL);
    assert(test_map_lookup(table, string_ref_create("c")) != NULL);

    test_map_free(table);
}

static void test_delete_all() {
    TestMap *table = test_map_create(8);

    StringRef keys[] = {string_ref_create("a"), string_ref_create("b"), string_ref_create("c"),
                        string_ref_create("d")};
    for (int i = 0; i < 4; i++) {
        assert(test_map_insert(table, keys[i], i) == true);
    }
    assert(table->size == 4);

    for (int i = 0; i < 4; i++) {
        test_map_delete(table, keys[i]);
    }
    assert(table->size == 0);

    for (int i = 0; i < 4; i++) {
        assert(test_map_lookup(table, keys[i]) == NULL);
    }

    test_map_free(table);
}

static void test_stress() {
    TestMap *table = test_map_create(16);
    const int NUM_ENTRIES = 1000;

    for (int i = 0; i < NUM_ENTRIES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "var%d", i);
        assert(test_map_insert(table, string_ref_create(key), i) == true);
    }
    assert(table->size == NUM_ENTRIES);

    for (int i = 0; i < NUM_ENTRIES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "var%d", i);
        int *entry = test_map_lookup(table, string_ref_create(key));
        assert(entry != NULL);
        assert(*entry == i);
    }

    for (int i = 0; i < NUM_ENTRIES; i += 2) {
        char key[16];
        snprintf(key, sizeof(key), "var%d", i);
        test_map_delete(table, string_ref_create(key));
    }

    assert(table->size == NUM_ENTRIES / 2);

    test_map_free(table);
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
