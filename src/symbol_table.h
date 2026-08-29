#ifndef GAB_SYMBOL_TABLE_H
#define GAB_SYMBOL_TABLE_H

#include "scope.h"
#include "string/string.h"
#include "type/type.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stddef.h>

#define SYMBOL_TABLE_INITIAL_CAPACITY 8

// A function symbol that has been declared but has no body yet.
#define SYMBOL_FUNC_NO_BODY ((size_t)-1)

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

    // Set when 'ref x' is taken. A pinned variable's slot must survive its whole
    // block, so codegen may not reclaim it at the end of a statement.
    bool pinned;

    union {
        struct {
            const Type *type;
        } var;

        struct {
            const Type *return_type;

            const Type **params;
            size_t param_count;

            // Which function this names, in the VM's list of them. Unlike a
            // frame slot this is durable output: it is what a call encodes and
            // what gab_call resolves a handle through, long after the compile
            // that made it. SYMBOL_FUNC_NO_BODY until codegen emits a body.
            size_t func_index;

            // Declared 'extern': the body lives in the host, and the prototype
            // codegen reserves carries a C function pointer instead of a chunk.
            bool is_extern;

            // What the host binds a body to. Only an extern carries these: a
            // symbol is otherwise found by the name it is stored under, and
            // nothing needs to ask a symbol what it is called.
            String *name;
            String *module;

            // The C body, for an extern whose body is known where it is
            // declared rather than looked up by name at link -- which is what a
            // generic instantiation's methods are. A void pointer because what
            // a C body is belongs to the VM's header, which this one is reached
            // from rather than reaching. NULL for an extern a host supplies.
            void *body;
        } func;
    };
} Symbol;

#define symbol_table_hash(key) (size_t)key
#define symbol_table_key_equals(key, other) key == other
#define symbol_table_key_dup(key) key
#define symbol_table_entry_free(key, value)

GAB_HASH_MAP(SymbolTable, symbol_table, String *, Symbol *);

#endif
