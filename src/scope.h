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

    struct Scope *parent;
    int depth;
} Scope;

// Initialize a new scope
Scope *scope_create(Arena *arena, Scope *parent);
void scope_init(Scope *scope, Arena *arena, Scope *parent);

Symbol *scope_symbol_lookup(Scope *scope, String *name);
Symbol *scope_decl_var(Scope *scope, String *name, Type *type);
Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type);

#endif
