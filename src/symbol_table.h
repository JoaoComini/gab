#ifndef GAB_SYMBOL_TABLE_H
#define GAB_SYMBOL_TABLE_H

#include "scope.h"
#include "string/string.h"
#include "type.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stddef.h>

#define SYMBOL_TABLE_INITIAL_CAPACITY 8

// A function symbol that has been declared but has no compiled body yet.
#define SYMBOL_FUNC_NO_PROTO ((size_t)-1)

typedef enum {
    SYMBOL_VAR,
    SYMBOL_FUNC,
} SymbolKind;

// What a name means. Written by the resolver and read for as long as the VM
// lives, so nothing here may be true of only one compile: frame slots, which
// are, live in codegen's own side table instead.
typedef struct Symbol {
    SymbolKind kind;

    int scope_depth;

    // The compile that declared this. A top-level symbol outlives the compile
    // that made it, so a later compile meeting the same name needs to know
    // whether it is looking at its own work — a duplicate declaration — or at
    // an earlier compile's, which it is entitled to replace.
    unsigned int generation;

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

            // Index into the VM's prototype list. Unlike a frame slot this is
            // durable output: it is what OP_CALL encodes and what gab_call
            // resolves a handle through, long after the compile that made it.
            // SYMBOL_FUNC_NO_PROTO until codegen emits a body.
            size_t proto_index;

            // Declared 'extern': the body lives in the host, and the prototype
            // codegen reserves carries a C function pointer instead of a chunk.
            bool is_extern;

            // What the host binds a body to. Only an extern carries these: a
            // symbol is otherwise found by the name it is stored under, and
            // nothing needs to ask a symbol what it is called.
            String *name;
            String *module;
        } func;
    };
} Symbol;

#define symbol_table_hash(key) (size_t)key
#define symbol_table_key_equals(key, other) key == other
#define symbol_table_key_dup(key) key
#define symbol_table_entry_free(key, value)

GAB_HASH_MAP(SymbolTable, symbol_table, String *, Symbol *);

#endif
