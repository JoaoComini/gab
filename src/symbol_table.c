#include "symbol_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t djb2_hash(const char *str) {
    size_t hash = 5381;
    int ch;
    while ((ch = *str++)) {
        hash = ((hash << 5) + hash) + ch;
    }
    return hash;
}

static void symbol_table_resize(SymbolTable *table) {
    int new_capacity = table->capacity * 2;
    SymbolEntry **new_buckets = calloc(new_capacity, sizeof(SymbolEntry *));

    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry *entry = table->buckets[i];
        while (entry) {
            SymbolEntry *next = entry->next;
            size_t new_idx = djb2_hash(entry->key) % new_capacity;

            entry->next = new_buckets[new_idx];
            new_buckets[new_idx] = entry;

            entry = next;
        }
    }

    free(table->buckets);
    table->buckets = new_buckets;
    table->capacity = new_capacity;
}

SymbolTable *symbol_table_create(int initial_capacity) {
    SymbolTable *table = malloc(sizeof(SymbolTable));
    table->buckets = calloc(initial_capacity, sizeof(SymbolEntry *));
    table->capacity = initial_capacity;
    table->size = 0;

    return table;
}

void symbol_table_free(SymbolTable *table) {
    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry *entry = table->buckets[i];
        while (entry) {
            SymbolEntry *next = entry->next;
            free(entry->key);
            free(entry);
            entry = next;
        }
    }
    free(table->buckets);
    free(table);
}

bool symbol_table_insert(SymbolTable *table, const char *key, size_t reg) {
    if (table->size >= table->capacity * SYMBOL_TABLE_RESIZE_THRESHOLD) {
        symbol_table_resize(table);
    }

    size_t idx = djb2_hash(key) % table->capacity;
    SymbolEntry *entry = table->buckets[idx];

    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            return false;
        }
        entry = entry->next;
    }

    SymbolEntry *new_entry = malloc(sizeof(SymbolEntry));
    new_entry->key = strdup(key);
    new_entry->symbol.reg = reg;
    new_entry->next = table->buckets[idx];
    table->buckets[idx] = new_entry;
    table->size++;

    return true;
}

SymbolEntry *symbol_table_lookup(SymbolTable *table, const char *key) {
    size_t idx = djb2_hash(key) % table->capacity;

    SymbolEntry *entry = table->buckets[idx];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

void symbol_table_delete(SymbolTable *table, const char *key) {
    size_t idx = djb2_hash(key) % table->capacity;
    SymbolEntry *entry = table->buckets[idx];
    SymbolEntry *prev = NULL;

    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            if (prev) {
                prev->next = entry->next;
            } else {
                table->buckets[idx] = entry->next;
            }
            free(entry->key);
            free(entry);
            table->size--;
        }
        prev = entry;
        entry = entry->next;
    }
}
