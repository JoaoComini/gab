#include "scope.h"
#include "arena.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type_registry.h"
#include <assert.h>

Scope *scope_create(Arena *arena, StringPool *strings, Scope *parent) {
    Scope *scope = arena_alloc(arena, sizeof(Scope));
    scope_init(scope, arena, strings, parent);
    return scope;
}

void scope_init(Scope *scope, Arena *arena, StringPool *strings, Scope *parent) {
    scope_init_at_depth(scope, arena, strings, parent, parent ? parent->depth + 1 : 0);
}

// The builtin type names, declared into the root scope so that 'int' resolves
// by the ordinary chain walk from wherever it is written, with no special case
// in the resolver. The registry still owns the Types themselves; this only
// gives them names to be found by.
static void scope_declare_builtins(Scope *scope) {
    const TypeBuiltins *builtins = &scope->type_registry->builtins;

    scope_decl_type(scope, builtins->int_type->name, builtins->int_type);
    scope_decl_type(scope, builtins->float_type->name, builtins->float_type);
    scope_decl_type(scope, builtins->bool_type->name, builtins->bool_type);
}

void scope_set_generation(Scope *scope, unsigned int generation) { scope->generation = generation; }

void scope_init_at_depth(Scope *scope, Arena *arena, StringPool *strings, Scope *parent, int depth) {
    scope->arena = arena;
    scope->strings = strings;
    scope->symbol_table = symbol_table_create_alloc(arena_allocator(arena), SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->types = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    scope->parent = parent;
    scope->depth = depth;

    // Inherited, so a block declares at the same generation as the unit it is
    // part of: two 'let x' in one block are still a duplicate, and a block
    // scope is fresh per compile anyway.
    scope->generation = parent ? parent->generation : 0;

    // One registry for the whole chain: it interns, and interning must be
    // single however deep the scope is.
    if (parent && parent->type_registry) {
        scope->type_registry = parent->type_registry;
        return;
    }

    scope->type_registry = type_registry_create(arena, strings);
    scope_declare_builtins(scope);
}

void scope_init_module(Scope *scope, Arena *arena, StringPool *strings, Scope *parent) {
    assert(parent && "a module scope hangs off the root scope");

    // Depth 0 despite being nested: its declarations are a unit's top level,
    // not a block. Depth drives the pointer-lifetime rule, where 0 means
    // 'outlives everything'.
    scope_init_at_depth(scope, arena, strings, parent, 0);
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

Type *scope_type_lookup(Scope *scope, String *name) {
    while (scope) {
        TypeBinding *entry = type_map_lookup(scope->types, name);
        if (entry) {
            return entry->type;
        }

        scope = scope->parent;
    }

    return NULL;
}

Type *scope_type_lookup_local(Scope *scope, String *name) {
    TypeBinding *entry = type_map_lookup(scope->types, name);

    return entry ? entry->type : NULL;
}

bool scope_declares_type_now(Scope *scope, String *name) {
    TypeBinding *entry = type_map_lookup(scope->types, name);

    return entry && entry->generation == scope->generation;
}

void scope_withdraw_type(Scope *scope, String *name) { type_map_delete(scope->types, name); }

bool scope_decl_type(Scope *scope, String *name, Type *type) {
    TypeBinding binding = {.type = type, .generation = scope->generation};

    // Only this compile's own declaration is a duplicate. An earlier one is
    // rebound, which is what lets a unit declaring a struct be recompiled.
    TypeBinding *existing = type_map_lookup(scope->types, name);

    if (existing) {
        if (existing->generation == scope->generation) {
            return false;
        }

        // insert does not overwrite, so rebinding is a write through the entry
        // the lookup found.
        *existing = binding;

        return true;
    }

    type_map_insert(scope->types, name, binding);

    return true;
}

Symbol *scope_decl_var(Scope *scope, String *name, Type *type) {
    // As scope_decl_func: an older generation is a previous compile's
    // declaration, which recompiling replaces in place.
    Symbol **existing = symbol_table_lookup(scope->symbol_table, name);

    if (existing) {
        Symbol *sym = *existing;

        if (sym->generation == scope->generation) {
            return NULL;
        }

        sym->kind = SYMBOL_VAR;
        sym->scope_depth = scope->depth;
        sym->pinned = false;
        sym->generation = scope->generation;
        sym->var.type = type;
        sym->var.pointee_depth = 0;

        return sym;
    }

    Symbol *sym = arena_alloc(scope->arena, sizeof(Symbol));
    sym->kind = SYMBOL_VAR;
    sym->scope_depth = scope->depth;
    sym->pinned = false;
    sym->generation = scope->generation;
    sym->var.type = type;
    sym->var.pointee_depth = 0;

    Symbol **decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}

Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type) {
    // A name this scope already holds is a duplicate only if this same compile
    // declared it. An older generation is a previous compile's, and recompiling
    // a unit is expected to replace what it declared last time.
    //
    // The existing Symbol is reused rather than replaced, because a host's
    // GabFunc handle points at it: allocating a fresh one would leave every
    // live handle addressing a symbol nothing declares any more.
    Symbol **existing = symbol_table_lookup(scope->symbol_table, name);

    if (existing) {
        Symbol *sym = *existing;

        if (sym->generation == scope->generation) {
            return NULL;
        }

        sym->kind = SYMBOL_FUNC;
        sym->scope_depth = scope->depth;
        sym->pinned = false;
        sym->generation = scope->generation;
        sym->func.return_type = return_type;
        sym->func.params = NULL;
        sym->func.param_count = 0;
        sym->func.proto_index = SYMBOL_FUNC_NO_PROTO;

        return sym;
    }

    Symbol *sym = arena_alloc(scope->arena, sizeof(Symbol));
    sym->kind = SYMBOL_FUNC;
    sym->scope_depth = scope->depth;
    sym->pinned = false;
    sym->generation = scope->generation;
    sym->func.return_type = return_type;
    sym->func.params = NULL;
    sym->func.param_count = 0;
    sym->func.proto_index = SYMBOL_FUNC_NO_PROTO;

    Symbol **decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}
