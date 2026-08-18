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

void scope_init_at_depth(Scope *scope, Arena *arena, StringPool *strings, Scope *parent, int depth) {
    scope->arena = arena;
    scope->strings = strings;
    scope->symbol_table = symbol_table_create_alloc(arena_allocator(arena), SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->types = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    scope->parent = parent;
    scope->depth = depth;

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
        Type **entry = type_map_lookup(scope->types, name);
        if (entry) {
            return *entry;
        }

        scope = scope->parent;
    }

    return NULL;
}

Type *scope_type_lookup_local(Scope *scope, String *name) {
    Type **entry = type_map_lookup(scope->types, name);

    return entry ? *entry : NULL;
}

bool scope_decl_type(Scope *scope, String *name, Type *type) {
    if (scope_type_lookup_local(scope, name)) {
        return false;
    }

    type_map_insert(scope->types, name, type);

    return true;
}

Symbol *scope_decl_var(Scope *scope, String *name, Type *type) {
    Symbol *sym = arena_alloc(scope->arena, sizeof(Symbol));
    sym->kind = SYMBOL_VAR;
    sym->scope_depth = scope->depth;
    sym->pinned = false;
    sym->var.type = type;
    sym->var.pointee_depth = 0;

    Symbol **decl = symbol_table_insert(scope->symbol_table, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}

Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type) {
    Symbol *sym = arena_alloc(scope->arena, sizeof(Symbol));
    sym->kind = SYMBOL_FUNC;
    sym->scope_depth = scope->depth;
    sym->pinned = false;
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
