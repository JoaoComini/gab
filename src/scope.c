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
    scope_decl_type(scope, builtins->string_type->name, builtins->string_type);
    scope_decl_type(scope, builtins->str_type->name, builtins->str_type);

    // The bare name. Never a usable type on its own -- resolving a spec turns
    // it into the 'Array T' its element names -- but it must be found here for
    // that spec to get as far as applying one.
    scope_decl_type(scope, builtins->array_type->name, builtins->array_type);
}

void scope_init_at_depth(Scope *scope, Arena *arena, StringPool *strings, Scope *parent, int depth) {
    scope->arena = arena;
    scope->strings = strings;
    scope->symbol_table = symbol_table_create_alloc(arena_allocator(arena), SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->types = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    scope->parent = parent;
    scope->depth = depth;
    scope->declares_module = false;

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

    scope->declares_module = true;
}

// A scope a compile declares into instead of the one it is for, so nothing it
// declares is visible until the compile has wholly succeeded.
//
// Parented to the target, which is what lets the compile still see everything
// already declared there and further out. The depth is the target's, not one
// deeper: a staging scope stands in for its target rather than nesting inside
// it, and depth drives the pointer-lifetime rule.
void scope_init_staging(Scope *scope, Arena *arena, StringPool *strings, Scope *target) {
    assert(target && "a staging scope stands in for a scope that exists");

    scope_init_at_depth(scope, arena, strings, target, target->depth);

    // Stands in for the target, so a declaration searches the two together.
    scope->declares_module = target->declares_module;
}

// Moves everything a staging scope declared into the scope it stood in for.
// Cannot collide: a name the module already holds was refused where the unit
// declared it, so nothing here can fail.
void scope_merge_staged(Scope *target, Scope *staged) {
    for (size_t i = 0; i < staged->symbol_table->capacity; i++) {
        for (SymbolTableEntry *entry = staged->symbol_table->buckets[i]; entry; entry = entry->next) {
            symbol_table_insert(target->symbol_table, entry->key, entry->value);
        }
    }

    for (size_t i = 0; i < staged->types->capacity; i++) {
        for (TypeMapEntry *entry = staged->types->buckets[i]; entry; entry = entry->next) {
            type_map_insert(target->types, entry->key, entry->value);
        }
    }
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

// A type declaration clashes with this scope, with the module scope a staging
// scope stands in for, and with the root -- so a unit may not name a struct
// 'int'. A builtin is reachable from every module and there is no qualified
// syntax to reach past a shadow, so shadowing one would put it out of reach for
// the rest of the module, including in a signature a host resolves through.
Type *scope_type_lookup_declaring(Scope *scope, String *name) { return scope_type_lookup(scope, name); }

Symbol *scope_symbol_lookup_declaring(Scope *scope, String *name) {
    for (Scope *s = scope;; s = s->parent) {
        Symbol **entry = symbol_table_lookup(s->symbol_table, name);

        if (entry) {
            return *entry;
        }

        if (!s->declares_module || !s->parent || !s->parent->declares_module) {
            return NULL;
        }
    }
}

void scope_withdraw_type(Scope *scope, String *name) { type_map_delete(scope->types, name); }

bool scope_decl_type(Scope *scope, String *name, Type *type) {
    if (type_map_lookup(scope->types, name)) {
        return false;
    }

    type_map_insert(scope->types, name, (TypeBinding){.type = type});

    return true;
}

Symbol *scope_decl_var(Scope *scope, String *name, Type *type) {
    if (scope_symbol_lookup_declaring(scope, name)) {
        return NULL;
    }

    Symbol *sym = arena_alloc(scope->arena, sizeof(Symbol));
    sym->kind = SYMBOL_VAR;
    sym->scope_depth = scope->depth;
    sym->pinned = false;
    sym->var.type = type;

    Symbol **decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}

Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type) {
    if (scope_symbol_lookup_declaring(scope, name)) {
        return NULL;
    }

    Symbol *sym = arena_alloc(scope->arena, sizeof(Symbol));
    sym->kind = SYMBOL_FUNC;
    sym->scope_depth = scope->depth;
    sym->pinned = false;
    sym->func.return_type = return_type;
    sym->func.params = NULL;
    sym->func.param_count = 0;
    sym->func.func_index = SYMBOL_FUNC_NO_BODY;
    sym->func.is_extern = false;

    Symbol **decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}
