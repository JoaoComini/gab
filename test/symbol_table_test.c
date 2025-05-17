#include "symbol_table.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_create_and_free() {
    SymbolTable *table = symbol_table_create(8);
    assert(table != NULL);
    assert(table->capacity == 8);
    assert(table->size == 0);
    symbol_table_free(table);
}

static void test_basic_insert_and_lookup() {
    SymbolTable *table = symbol_table_create(8);

    assert(symbol_table_insert(table, "x", 1) == true);
    assert(table->size == 1);
    assert(symbol_table_insert(table, "y", 2) == true);
    assert(table->size == 2);

    SymbolEntry *x = symbol_table_lookup(table, "x");
    assert(x != NULL);
    assert(strcmp(x->key, "x") == 0);
    assert(x->reg == 1);

    SymbolEntry *y = symbol_table_lookup(table, "y");
    assert(y != NULL);
    assert(strcmp(y->key, "y") == 0);
    assert(y->reg == 2);

    assert(symbol_table_lookup(table, "z") == NULL);

    symbol_table_free(table);
}

static void test_duplicate_insert() {
    SymbolTable *table = symbol_table_create(8);

    assert(symbol_table_insert(table, "x", 1) == true);
    assert(table->size == 1);

    assert(symbol_table_insert(table, "x", 2) == false);
    assert(table->size == 1); // Size shouldn't change

    SymbolEntry *x = symbol_table_lookup(table, "x");
    assert(x->reg == 1);

    symbol_table_free(table);
}

static void test_delete() {
    SymbolTable *table = symbol_table_create(8);

    assert(symbol_table_insert(table, "x", 1) == true);
    assert(symbol_table_insert(table, "y", 2) == true);
    assert(table->size == 2);

    symbol_table_delete(table, "x");
    assert(table->size == 1);
    assert(symbol_table_lookup(table, "x") == NULL);
    assert(symbol_table_lookup(table, "y") != NULL);

    symbol_table_delete(table, "z");
    assert(table->size == 1);

    symbol_table_free(table);
}

static void test_collision_handling() {
    SymbolTable *table = symbol_table_create(2);

    assert(symbol_table_insert(table, "a", 1) == true);
    assert(symbol_table_insert(table, "b", 2) == true);
    assert(symbol_table_insert(table, "c", 3) == true);
    assert(table->size == 3);

    assert(symbol_table_lookup(table, "a") != NULL);
    assert(symbol_table_lookup(table, "b") != NULL);
    assert(symbol_table_lookup(table, "c") != NULL);

    symbol_table_free(table);
}

static void test_resize() {
    SymbolTable *table = symbol_table_create(4);

    assert(table->capacity == 4);
    assert(symbol_table_insert(table, "a", 1) == true);
    assert(symbol_table_insert(table, "b", 2) == true);
    assert(symbol_table_insert(table, "c", 3) == true);

    assert(table->capacity == 8);
    assert(table->size == 3);

    assert(symbol_table_lookup(table, "a") != NULL);
    assert(symbol_table_lookup(table, "b") != NULL);
    assert(symbol_table_lookup(table, "c") != NULL);

    symbol_table_free(table);
}

static void test_delete_all() {
    SymbolTable *table = symbol_table_create(8);

    const char *keys[] = {"a", "b", "c", "d"};
    for (int i = 0; i < 4; i++) {
        assert(symbol_table_insert(table, keys[i], i) == true);
    }
    assert(table->size == 4);

    for (int i = 0; i < 4; i++) {
        symbol_table_delete(table, keys[i]);
    }
    assert(table->size == 0);

    for (int i = 0; i < 4; i++) {
        assert(symbol_table_lookup(table, keys[i]) == NULL);
    }

    symbol_table_free(table);
}

static void test_edge_cases() {
    SymbolTable *table = symbol_table_create(8);

    assert(symbol_table_insert(table, "", 0) == true);
    SymbolEntry *empty = symbol_table_lookup(table, "");
    assert(empty != NULL);
    assert(strcmp(empty->key, "") == 0);

    symbol_table_free(table);
}

static void test_stress() {
    SymbolTable *table = symbol_table_create(16);
    const int NUM_ENTRIES = 1000;

    for (int i = 0; i < NUM_ENTRIES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "var%d", i);
        assert(symbol_table_insert(table, key, i) == true);
    }
    assert(table->size == NUM_ENTRIES);

    for (int i = 0; i < NUM_ENTRIES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "var%d", i);
        SymbolEntry *entry = symbol_table_lookup(table, key);
        assert(entry != NULL);
        assert(entry->reg == i);
    }

    for (int i = 0; i < NUM_ENTRIES; i += 2) {
        char key[16];
        snprintf(key, sizeof(key), "var%d", i);
        symbol_table_delete(table, key);
    }

    assert(table->size == NUM_ENTRIES / 2);

    symbol_table_free(table);
}

int main() {
    test_create_and_free();
    test_basic_insert_and_lookup();
    test_duplicate_insert();
    test_delete();
    test_collision_handling();
    test_resize();
    test_delete_all();
    test_edge_cases();
    test_stress();

    return 0;
}
