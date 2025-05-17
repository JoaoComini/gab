#ifndef GAB_SYMBOL_TABLE_H
#define GAB_SYMBOL_TABLE_H

#include <stdbool.h>
#include <stddef.h>

#define SYMBOL_TABLE_RESIZE_THRESHOLD 0.5
#define SYMBOL_TABLE_INITIAL_CAPACITY 8

typedef struct SymbolEntry {
    char *key; // Variable name (owned by the entry)
    int reg;   // Associated register
    struct SymbolEntry *next;
} SymbolEntry;

typedef struct {
    SymbolEntry **buckets;
    int capacity;
    int size;
} SymbolTable;

SymbolTable *symbol_table_create(int initial_capacity);

void symbol_table_free(SymbolTable *table);

bool symbol_table_insert(SymbolTable *table, const char *key, int reg);

SymbolEntry *symbol_table_lookup(SymbolTable *table, const char *key);

void symbol_table_delete(SymbolTable *table, const char *key);

#endif
