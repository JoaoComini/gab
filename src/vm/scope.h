#ifndef GAB_SCOPE_H
#define GAB_SCOPE_H

#include "symbol_table.h"

typedef struct Scope {
    SymbolTable *symbol_table;
    int next_reg;

    struct Scope *parent;
} Scope;

// Initialize a new scope
Scope *scope_create(Scope *parent);

// Free a scope (and its symbol table)
void scope_free(Scope *scope);

// Allocate a new register
int scope_alloc_register(Scope *scope);
void scope_free_register(Scope *scope);

SymbolEntry *scope_symbol_lookup(Scope *scope, const char *name);

#endif
