#include "scope.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type_registry.h"
#include <assert.h>
#include <stdlib.h>

Scope *scope_create(Scope *parent) {
    Scope *scope = malloc(sizeof(Scope));
    scope_init(scope, parent);
    return scope;
}

void scope_init(Scope *scope, Scope *parent) {
    scope->symbol_table = symbol_table_create(SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->type_registry = parent && parent->type_registry ? parent->type_registry : type_registry_create();
    scope->parent = parent;
    scope->depth = parent ? parent->depth + 1 : 0;
}

void scope_destroy(Scope *scope) {
    scope_free(scope);
    free(scope);
}

void scope_free(Scope *scope) {
    symbol_table_destroy(scope->symbol_table);

    if (scope->parent) {
        return;
    }

    type_registry_destroy(scope->type_registry);
}

Symbol *scope_symbol_lookup(Scope *scope, String *name) {
    while (scope) {
        Symbol **entry = symbol_table_lookup(scope->symbol_table, name);
        if (entry) {
            return *entry;
        }

        scope = scope->parent;
    }

    return NULL;
}

Symbol *scope_decl_var(Scope *scope, String *name, Type *type) {
    Symbol *sym = malloc(sizeof(Symbol));
    sym->scope_depth = scope->depth;
    sym->var.type = type;

    Symbol **decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}

Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type) {
    Symbol *sym = malloc(sizeof(Symbol));
    sym->scope_depth = scope->depth;
    sym->func.return_type = return_type;

    Symbol **decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}
