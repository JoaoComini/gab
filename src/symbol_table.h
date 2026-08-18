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

    // Set when '&x' is taken. A pinned variable's slot must survive its whole
    // block, so codegen may not reclaim it at the end of a statement.
    bool pinned;

    union {
        struct {
            Type *type;

            // For a pointer variable, the block depth of what it points at, or
            // 0 when that is not known. A pointer may only be moved to a depth
            // at least this deep: anything shallower outlives its pointee.
            int pointee_depth;
        } var;

        struct {
            Type *return_type;

            Type **params;
            size_t param_count;
        } func;
    };
} Symbol;

#define symbol_table_hash(key) (size_t)key
#define symbol_table_key_equals(key, other) key == other
#define symbol_table_key_dup(key) key
#define symbol_table_entry_free(key, value)

GAB_HASH_MAP(SymbolTable, symbol_table, String *, Symbol *);

#endif
