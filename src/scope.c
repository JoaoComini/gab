#include "scope.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type_registry.h"
#include <assert.h>
#include <stdlib.h>

Scope *scope_create(ScopeKind kind, Scope *parent) {
    Scope *scope = malloc(sizeof(Scope));
    scope_init(scope, kind, parent);
    return scope;
}

void scope_init(Scope *scope, ScopeKind kind, Scope *parent) {
    scope->kind = kind;
    scope->symbol_table = symbol_table_create(SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->type_registry = parent && parent->type_registry ? parent->type_registry : type_registry_create();
    scope->var_offset = parent ? parent->var_offset : 0;
    scope->parent = parent;
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
        Symbol *entry = symbol_table_lookup(scope->symbol_table, name);
        if (entry) {
            return entry;
        }

        scope = scope->parent;
    }

    return NULL;
}

Symbol *scope_decl_var(Scope *scope, String *name, Type *type) {
    Symbol sym;
    sym.scope = scope->kind;
    sym.offset = scope->var_offset;
    sym.var.type = type;

    Symbol *decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    scope->var_offset++;

    return decl;
}

Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type) {
    Symbol sym;
    sym.scope = scope->kind;
    sym.func.return_type = return_type;

    Symbol *decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return decl;
}
