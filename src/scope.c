#include "scope.h"
#include "arena.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type/type_registry.h"
#include <assert.h>

Scope *scope_create(Arena *arena, StringPool *strings, Scope *parent) {
    Scope *scope = arena_alloc(arena, sizeof(Scope));
    scope_init(scope, arena, strings, parent);
    return scope;
}

void scope_init(Scope *scope, Arena *arena, StringPool *strings, Scope *parent) {
    scope_init_at_depth(scope, arena, strings, parent, parent ? parent->depth + 1 : 0);
}

static void scope_declare_primitives(Scope *scope) {
    TypeRegistry *registry = scope->type_registry;

    static const TypeKind PRIMITIVES[] = {TYPE_INT, TYPE_FLOAT, TYPE_BOOL, TYPE_STR};

    for (size_t i = 0; i < sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]); i++) {
        const Type *type = type_registry_get_primitive(registry, PRIMITIVES[i]);

        scope_bind_type(scope, type_name_of(type), type);
    }
}

void scope_init_at_depth(Scope *scope, Arena *arena, StringPool *strings, Scope *parent, int depth) {
    scope->arena = arena;
    scope->strings = strings;
    scope->symbol_table = symbol_table_create_alloc(arena_allocator(arena), SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->types = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    scope->parent = parent;
    scope->depth = depth;
    scope->declares_module = false;

    if (parent && parent->type_registry) {
        scope->type_registry = parent->type_registry;
        return;
    }

    const TypePrimitiveNames names = type_primitive_names(strings);

    scope->type_registry = type_registry_create(arena, &names);
    scope_declare_primitives(scope);
}

void scope_init_over(Scope *scope, Arena *arena, StringPool *strings, TypeRegistry *registry) {
    scope->arena = arena;
    scope->strings = strings;
    scope->symbol_table = symbol_table_create_alloc(arena_allocator(arena), SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->types = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    scope->parent = NULL;
    scope->depth = 0;
    scope->declares_module = false;

    scope->type_registry = registry;

    scope_declare_primitives(scope);
}

void scope_init_module(Scope *scope, Arena *arena, StringPool *strings, Scope *parent) {
    assert(parent && "a module scope hangs off the root scope");

    scope_init_at_depth(scope, arena, strings, parent, 0);

    scope->declares_module = true;
}

void scope_init_staging(Scope *scope, Arena *arena, StringPool *strings, Scope *target) {
    assert(target && "a staging scope stands in for a scope that exists");

    scope_init_at_depth(scope, arena, strings, target, target->depth);

    scope->declares_module = target->declares_module;
}

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

const Type *scope_type_lookup(Scope *scope, String *name) {
    return scope ? resolution_type(scope->type_registry, scope_resolve(scope, name)) : NULL;
}

Resolution scope_resolve(Scope *scope, String *name) {
    for (Scope *s = scope; s; s = s->parent) {
        TypeBinding *bound = type_map_lookup(s->types, name);

        if (bound) {
            if (!bound->def) {
                return (Resolution){.kind = RESOLUTION_TYPE, .type = bound->type};
            }

            return (Resolution){.kind = RESOLUTION_TYPE_DECL, .def = bound->def};
        }

        Symbol **symbol = symbol_table_lookup(s->symbol_table, name);

        if (symbol) {
            return (Resolution){.kind = RESOLUTION_VALUE, .symbol = *symbol};
        }
    }

    return (Resolution){.kind = RESOLUTION_NONE};
}

const Type *resolution_type(TypeRegistry *registry, Resolution resolution) {
    switch (resolution.kind) {
    case RESOLUTION_TYPE:
        return resolution.type;

    case RESOLUTION_TYPE_DECL:
        return resolution.def->param_count == 0 ? type_registry_apply(registry, resolution.def, NULL, 0)
                                                : NULL;

    default:
        return NULL;
    }
}

TypeBinding *scope_binding_lookup_local(Scope *scope, String *name) {
    return type_map_lookup(scope->types, name);
}

bool scope_declares_type(Scope *scope, String *name) {
    for (Scope *s = scope; s; s = s->parent) {
        if (type_map_lookup(s->types, name)) {
            return true;
        }
    }

    return false;
}

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

bool scope_bind_type(Scope *scope, String *name, const Type *type) {
    if (type_map_lookup(scope->types, name)) {
        return false;
    }

    assert(type_names_itself(type) && "a nominal name binds to what it declares");

    type_map_insert(scope->types, name, (TypeBinding){.type = type});

    return true;
}

bool scope_bind_decl(Scope *scope, String *name, const TypeDef *def) {
    if (type_map_lookup(scope->types, name)) {
        return false;
    }

    type_map_insert(scope->types, name, (TypeBinding){.def = def});

    return true;
}

Symbol *scope_decl_var(Scope *scope, String *name, const Type *type) {
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

Symbol *scope_decl_func(Scope *scope, String *name, const Type *return_type) {
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
