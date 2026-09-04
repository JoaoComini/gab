#include "scope.h"

#include "arena.h"
#include "binding.h"
#include "function_registry.h"
#include "string/string.h"
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
    scope->bindings = binding_table_create_alloc(arena_allocator(arena), BINDING_TABLE_INITIAL_CAPACITY);
    scope->types = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    scope->interfaces = interface_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    scope->parent = parent;
    scope->depth = depth;
    scope->declares_module = false;

    if (parent && parent->type_registry) {
        scope->type_registry = parent->type_registry;
        scope->functions = parent->functions;
        return;
    }

    const TypePrimitiveNames names = type_primitive_names(strings);

    scope->type_registry = type_registry_create(arena, &names);
    scope->functions = function_registry_create(arena, scope->type_registry);
    scope_declare_primitives(scope);
}

void scope_init_over(Scope *scope, Arena *arena, StringPool *strings, TypeRegistry *registry) {
    scope->arena = arena;
    scope->strings = strings;
    scope->bindings = binding_table_create_alloc(arena_allocator(arena), BINDING_TABLE_INITIAL_CAPACITY);
    scope->types = type_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    scope->interfaces = interface_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    scope->parent = NULL;
    scope->depth = 0;
    scope->declares_module = false;

    scope->type_registry = registry;
    scope->functions = function_registry_create(arena, registry);

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
    GAB_HASH_MAP_FOR_EACH(staged->bindings, entry) {
        binding_table_insert(target->bindings, entry->key, entry->value);
    }

    GAB_HASH_MAP_FOR_EACH(staged->types, entry) { type_map_insert(target->types, entry->key, entry->value); }

    GAB_HASH_MAP_FOR_EACH(staged->interfaces, entry) {
        interface_map_insert(target->interfaces, entry->key, entry->value);
    }
}

Binding *scope_binding_lookup(Scope *scope, String *name) {
    while (scope) {
        Binding **entry = binding_table_lookup(scope->bindings, name);
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
            if (!bound->decl) {
                return (Resolution){.kind = RESOLUTION_TYPE, .arg = bound->arg};
            }

            return (Resolution){.kind = RESOLUTION_TYPE_DECL, .decl = bound->decl};
        }

        Binding **binding = binding_table_lookup(s->bindings, name);

        if (binding) {
            return (Resolution){.kind = RESOLUTION_VALUE, .binding = *binding};
        }
    }

    return (Resolution){.kind = RESOLUTION_NONE};
}

const Type *resolution_type(TypeRegistry *registry, Resolution resolution) {
    switch (resolution.kind) {
    case RESOLUTION_TYPE:
        return resolution.arg.kind == TYPE_ARG_TYPE ? resolution.arg.type : NULL;

    case RESOLUTION_TYPE_DECL:
        return resolution.decl->param_count == 0 ? type_registry_apply(registry, resolution.decl, NULL, 0)
                                                 : NULL;

    default:
        return NULL;
    }
}

TypeBinding *scope_binding_lookup_local(Scope *scope, String *name) {
    TypeBinding *bound = type_map_lookup(scope->types, name);

    /* A staging scope stands in for the module scope beneath it, so both name what this module declares. */
    if (!bound && scope->parent && scope->depth == scope->parent->depth) {
        return type_map_lookup(scope->parent->types, name);
    }

    return bound;
}

bool scope_declares_type(Scope *scope, String *name) {
    for (Scope *s = scope; s; s = s->parent) {
        if (type_map_lookup(s->types, name)) {
            return true;
        }
    }

    return false;
}

Binding *scope_binding_lookup_declaring(Scope *scope, String *name) {
    for (Scope *s = scope;; s = s->parent) {
        Binding **entry = binding_table_lookup(s->bindings, name);

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

    type_map_insert(scope->types, name, (TypeBinding){.arg = {.kind = TYPE_ARG_TYPE, .type = type}});

    return true;
}

bool scope_bind_argument(Scope *scope, String *name, TypeArg arg) {
    if (type_map_lookup(scope->types, name)) {
        return false;
    }

    type_map_insert(scope->types, name, (TypeBinding){.arg = arg});

    return true;
}

bool scope_bind_interface(Scope *scope, String *name, Interface *interface) {
    if (interface_map_lookup(scope->interfaces, name)) {
        return false;
    }

    interface_map_insert(scope->interfaces, name, interface);

    return true;
}

Interface *scope_interface_lookup(Scope *scope, String *name) {
    for (Scope *s = scope; s; s = s->parent) {
        Interface **found = interface_map_lookup(s->interfaces, name);

        if (found) {
            return *found;
        }
    }

    return NULL;
}

bool scope_bind_decl(Scope *scope, String *name, const TypeDecl *decl) {
    if (type_map_lookup(scope->types, name)) {
        return false;
    }

    type_map_insert(scope->types, name, (TypeBinding){.decl = decl});

    return true;
}

Binding *scope_decl_var(Scope *scope, String *name, const Type *type) {
    if (scope_binding_lookup_declaring(scope, name)) {
        return NULL;
    }

    Binding *sym = arena_alloc(scope->arena, sizeof(Binding));
    sym->kind = BINDING_VAR;
    sym->scope_depth = scope->depth;
    sym->pinned = false;
    sym->var.type = type;

    Binding **decl = binding_table_insert(scope->bindings, name, sym);
    if (!decl) {
        return NULL;
    }

    return *decl;
}

Binding *scope_decl_func(Scope *scope, String *name, const Type *return_type) {
    if (scope_binding_lookup_declaring(scope, name)) {
        return NULL;
    }

    Binding *binding = arena_alloc(scope->arena, sizeof(Binding));
    binding->kind = BINDING_FUNC;
    binding->scope_depth = scope->depth;
    binding->pinned = false;

    FuncDecl *func_decl = arena_alloc(scope->arena, sizeof(FuncDecl));

    *func_decl = (FuncDecl){.name = name, .body_kind = BODY_GAB};

    binding->func = arena_alloc(scope->arena, sizeof(Function));

    *binding->func = (Function){
        .decl = func_decl,
        .return_type = return_type,
        .params = NULL,
        .param_count = 0,
        .func_index = FUNCTION_NO_BODY,
    };

    Binding **decl = binding_table_insert(scope->bindings, name, binding);
    if (!decl) {
        return NULL;
    }

    return *decl;
}

FuncSignature func_signature_instantiate(TypeRegistry *registry, Arena *arena, const FuncSignature *generic,
                                         const TypeArg *args, size_t arg_count) {
    FuncSignature out = {
        .return_type = type_registry_substitute(registry, generic->return_type, args, arg_count),
    };

    if (generic->param_count > 0) {
        out.params = arena_alloc(arena, generic->param_count * sizeof(const Type *));
        out.param_count = generic->param_count;

        for (size_t i = 0; i < generic->param_count; i++) {
            out.params[i] = type_registry_substitute(registry, generic->params[i], args, arg_count);
        }
    }

    return out;
}
