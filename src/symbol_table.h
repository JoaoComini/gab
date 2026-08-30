#ifndef GAB_SYMBOL_TABLE_H
#define GAB_SYMBOL_TABLE_H

#include "scope.h"
#include "string/string.h"
#include "type/type.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stddef.h>

#define SYMBOL_TABLE_INITIAL_CAPACITY 8

#define SYMBOL_FUNC_NO_BODY ((size_t)-1)

typedef enum {
    SYMBOL_VAR,
    SYMBOL_FUNC,
} SymbolKind;

typedef struct Function {
    const Type *return_type;

    const Type **params;
    size_t param_count;

    size_t func_index;

    bool is_extern;

    String *name;
    String *module;

    void *body;
} Function;

typedef struct Symbol {
    SymbolKind kind;

    int scope_depth;

    bool pinned;

    union {
        struct {
            const Type *type;
        } var;

        Function func;
    };
} Symbol;

#define symbol_table_hash(key) (size_t)key
#define symbol_table_key_equals(key, other) key == other
#define symbol_table_key_dup(key) key
#define symbol_table_entry_free(key, value)

GAB_HASH_MAP(SymbolTable, symbol_table, String *, Symbol *);

#endif
