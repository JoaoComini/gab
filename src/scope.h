#ifndef GAB_SCOPE_H
#define GAB_SCOPE_H

#include "arena.h"
#include "string/string.h"
#include "type_registry.h"

typedef struct SymbolTable SymbolTable;
typedef struct Symbol Symbol;

typedef struct Scope {
    Arena *arena;

    SymbolTable *symbol_table;
    TypeRegistry *type_registry;
    StringPool *strings;

    struct Scope *parent;
    int depth;
} Scope;

// Initialize a new scope
Scope *scope_create(Arena *arena, StringPool *strings, Scope *parent);
void scope_init(Scope *scope, Arena *arena, StringPool *strings, Scope *parent);

// As scope_init, but with the depth given rather than derived from the parent.
// For a module scope, which parents to the root for builtin lookup while still
// holding depth-0 top-level declarations.
void scope_init_at_depth(Scope *scope, Arena *arena, StringPool *strings, Scope *parent, int depth);

Symbol *scope_symbol_lookup(Scope *scope, String *name);
Symbol *scope_decl_var(Scope *scope, String *name, Type *type);
Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type);

#endif
