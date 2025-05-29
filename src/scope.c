#include "scope.h"
#include "arena.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type_registry.h"
#include <assert.h>

Scope *scope_create(Arena *arena, Scope *parent) {
    Scope *scope = arena_alloc(arena, sizeof(Scope));
    scope_init(scope, arena, parent);
    return scope;
}

void scope_init(Scope *scope, Arena *arena, Scope *parent) {
    scope->arena = arena;
    scope->symbol_table = symbol_table_create_alloc(arena_allocator(arena), SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->type_registry =
        parent && parent->type_registry ? parent->type_registry : type_registry_create(arena);
    scope->parent = parent;
    scope->depth = parent ? parent->depth + 1 : 0;
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
    Symbol *sym = arena_alloc(scope->arena, sizeof(Symbol));
    sym->scope_depth = scope->depth;
    sym->var.type = type;

    Symbol **decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}

Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type) {
    Symbol *sym = arena_alloc(scope->arena, sizeof(Symbol));
    sym->scope_depth = scope->depth;
    sym->func.return_type = return_type;

    Symbol **decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}
