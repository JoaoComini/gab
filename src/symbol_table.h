#ifndef GAB_SYMBOL_TABLE_H
#define GAB_SYMBOL_TABLE_H

#include "scope.h"
#include "string/string.h"
#include "type.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stddef.h>

#define SYMBOL_TABLE_INITIAL_CAPACITY 8

typedef enum {
    SYMBOL_VAR,
    SYMBOL_FUNC,
} SymbolKind;

typedef struct Symbol {
    SymbolKind kind;

    unsigned int offset;
    int scope_depth;

    union {
        struct {
            Type *type;
        } var;

        struct {
            FuncSignature *signature;
        } func;
    };
} Symbol;

#define symbol_table_hash(key) (size_t)key
#define symbol_table_key_equals(key, other) key == other
#define symbol_table_key_dup(key) key
#define symbol_table_entry_free(key, value)

GAB_HASH_MAP(SymbolTable, symbol_table, String *, Symbol *);

#endif
