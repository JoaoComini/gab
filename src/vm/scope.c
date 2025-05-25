#include "scope.h"
#include "string_ref.h"
#include "symbol_table.h"
#include "vm/vm.h"
#include <assert.h>
#include <stdlib.h>

Scope *scope_create(Scope *parent) {
    Scope *scope = malloc(sizeof(Scope));
    scope->symbol_table = symbol_table_create(SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->next_reg = parent ? parent->next_reg : 0;
    scope->parent = parent;
    return scope;
}

void scope_free(Scope *scope) {
    free(scope->symbol_table);
    free(scope);
}

int scope_alloc_register(Scope *scope) {
    assert(scope->next_reg < VM_MAX_REGISTERS);
    return scope->next_reg++;
}

void scope_free_register(Scope *scope) {
    assert(scope->next_reg > 0);
    scope->next_reg--;
}

Symbol *scope_symbol_lookup(Scope *scope, StringRef name) {
    while (scope) {
        Symbol *entry = symbol_table_lookup(scope->symbol_table, name);
        if (entry) {
            return entry;
        }

        scope = scope->parent;
    }

    return NULL;
}
