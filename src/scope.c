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

// The builtin type names, declared into the root scope so that 'int' resolves
// by the ordinary chain walk from wherever it is written, with no special case
// in the resolver. The registry still owns the Types themselves; this only
// gives them names to be found by.
static void scope_declare_primitives(Scope *scope) {
    TypeRegistry *registry = scope->type_registry;

    static const TypeKind PRIMITIVES[] = {TYPE_INT, TYPE_FLOAT, TYPE_BOOL, TYPE_STR};

    for (size_t i = 0; i < sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]); i++) {
        const Type *type = type_registry_get_primitive(registry, PRIMITIVES[i]);

        scope_decl_type(scope, type_name_of(type), type);
    }

    // 'Array' is a declaration rather than a type: resolving a spec turns it
    // into the '[T; N]' its arguments name, and it must be found here for that
    // spec to get as far as applying one.
    const TypeDef *array_def = type_registry_array_def(registry);

    scope_decl_type_def(scope, array_def->name, array_def);

    // And nothing else. What a standard library provides is named where it is
    // declared -- see builtin_declare_type -- which is what keeps 'String' out
    // of the language and in the library that provides it.
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

    // Interned here rather than by the registry, which holds no pool: what
    // names a type is the same pool that names everything else in this scope.
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

    // The registry it was given, never one of its own: interning must be single
    // for the whole chain, and this one already holds what a library registered.
    scope->type_registry = registry;

    scope_declare_primitives(scope);
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

const Type *scope_type_lookup(Scope *scope, String *name) {
    return scope ? resolution_type(scope->type_registry, scope_resolve(scope, name)) : NULL;
}

Resolution scope_resolve(Scope *scope, String *name) {
    for (Scope *s = scope; s; s = s->parent) {
        TypeBinding *bound = type_map_lookup(s->types, name);

        if (bound) {
            // Told apart by which way the name was bound, never by asking what
            // the type is: scope_decl_type binds a type that stands for itself
            // and carries no declaration to apply, so the name answers with
            // that type. Everything nominal answers with its declaration,
            // whose arity is what a mention of it owes.
            if (!bound->def) {
                return (Resolution){.kind = RESOLUTION_SELF_NAMED, .self_named = bound->type};
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
    case RESOLUTION_SELF_NAMED:
        return resolution.self_named;

    // A declaration taking none is its own instantiation, which the registry
    // interned the first time anything named it.
    case RESOLUTION_TYPE_DECL:
        return resolution.def->param_count == 0 ? type_registry_instantiate(registry, resolution.def, NULL, 0)
                                                : NULL;

    default:
        return NULL;
    }
}

TypeBinding *scope_binding_lookup_local(Scope *scope, String *name) {
    return type_map_lookup(scope->types, name);
}

// A type declaration clashes with this scope, with the module scope a staging
// scope stands in for, and with the root -- so a unit may not name a struct
// 'int'. A builtin is reachable from every module and there is no qualified
// syntax to reach past a shadow, so shadowing one would put it out of reach for
// the rest of the module, including in a signature a host resolves through.
// Whether the name is bound at all rather than what it is bound to: a
// declaration still owed arguments names no type, and a unit redeclaring 'Vec'
// is as much a clash as one redeclaring 'int'.
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

bool scope_decl_type(Scope *scope, String *name, const Type *type) {
    if (type_map_lookup(scope->types, name)) {
        return false;
    }

    assert(type_names_itself(type) && "a nominal name binds to what it declares");

    // No declaration, which is what makes the binding the whole answer: a type
    // standing for itself was interned from none, and a lookup reads the
    // absence rather than asking the type what it is.
    type_map_insert(scope->types, name, (TypeBinding){.type = type});

    return true;
}

bool scope_decl_type_def(Scope *scope, String *name, const TypeDef *def) {
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
