#ifndef GAB_SYMBOL_TABLE_H
#define GAB_SYMBOL_TABLE_H

#include "hash_map.h"
#include "type.h"

#include <stdbool.h>
#include <stddef.h>

#define SYMBOL_TABLE_INITIAL_CAPACITY 8

typedef struct Symbol {
    unsigned int reg; // For locals/temporaries
    Type *type;
} Symbol;

GAB_HASH_MAP(SymbolTable, symbol_table, Symbol);

#endif
