#ifndef GAB_SCOPE_H
#define GAB_SCOPE_H

#include "string/string.h"
#include "type_registry.h"

typedef struct SymbolTable SymbolTable;
typedef struct Symbol Symbol;

typedef enum {
    SCOPE_GLOBAL,
    SCOPE_LOCAL,
} ScopeKind;

typedef struct Scope {
    ScopeKind kind;

    SymbolTable *symbol_table;
    TypeRegistry *type_registry;

    int var_offset;

    struct Scope *parent;
} Scope;

// Initialize a new scope
Scope *scope_create(ScopeKind kind, Scope *parent);
void scope_init(Scope *scope, ScopeKind kind, Scope *parent);

// Free a scope (and its symbol table)
void scope_destroy(Scope *scope);
void scope_free(Scope *scope);

Symbol *scope_symbol_lookup(Scope *scope, String *name);
Symbol *scope_decl_var(Scope *scope, String *name, Type *type);
Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type);

#endif
