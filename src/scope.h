#ifndef GAB_SCOPE_H
#define GAB_SCOPE_H

#include "string/string.h"
#include "symbol_table.h"

typedef struct Scope {
    SymbolTable *symbol_table;
    int var_offset;

    struct Scope *parent;
} Scope;

// Initialize a new scope
Scope *scope_create(Scope *parent);
void scope_init(Scope *scope, Scope *parent);

// Free a scope (and its symbol table)
void scope_destroy(Scope *scope);
void scope_free(Scope *scope);

Symbol *scope_symbol_lookup(Scope *scope, String *name);
Symbol *scope_decl_var(Scope *scope, String *name, Type *type);

#endif
