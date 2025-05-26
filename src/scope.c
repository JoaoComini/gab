#include "scope.h"
#include "string/string.h"
#include "symbol_table.h"
#include <assert.h>
#include <stdlib.h>

Scope *scope_create(Scope *parent) {
    Scope *scope = malloc(sizeof(Scope));
    scope_init(scope, parent);
    return scope;
}

void scope_init(Scope *scope, Scope *parent) {
    scope->symbol_table = symbol_table_create(SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->var_offset = parent ? parent->var_offset : 0;
    scope->parent = parent;
}

void scope_destroy(Scope *scope) {
    scope_free(scope);
    free(scope);
}

void scope_free(Scope *scope) { symbol_table_destroy(scope->symbol_table); }

Symbol *scope_symbol_lookup(Scope *scope, String *name) {
    while (scope) {
        Symbol *entry = symbol_table_lookup(scope->symbol_table, name);
        if (entry) {
            return entry;
        }

        scope = scope->parent;
    }

    return NULL;
}

Symbol *scope_decl_var(Scope *scope, String *name, Type *type) {
    Symbol var = (Symbol){.reg = scope->var_offset, type = type};

    Symbol *decl = symbol_table_insert(scope->symbol_table, name, var);
    if (!decl) {
        return NULL;
    }

    scope->var_offset++;

    return decl;
}
