#include "scope.h"

#include "decl.h"
#include "function_registry.h"
#include "memory/arena.h"
#include "string/string.h"
#include "type/type_registry.h"
#include <assert.h>

Scope *scope_create(Arena *arena, Scope *parent) {
    Scope *scope = arena_alloc(arena, sizeof(Scope));
    scope_init_kind(scope, arena, parent, parent ? SCOPE_LOCAL : SCOPE_GLOBAL);
    return scope;
}

void scope_init_kind(Scope *scope, Arena *arena, Scope *parent, ScopeKind kind) {
    scope->arena = arena;
    scope->symbols = symbol_table_create_alloc(arena_allocator(arena), SYMBOL_TABLE_INITIAL_CAPACITY);
    scope->parent = parent;
    scope->kind = kind;
}

Scope *scope_create_kind(Arena *arena, Scope *parent, ScopeKind kind) {
    Scope *scope = arena_alloc(arena, sizeof(Scope));
    scope_init_kind(scope, arena, parent, kind);
    return scope;
}

Scope *global_scope_create(Arena *arena, TypeRegistry *types) {
    Scope *scope = scope_create(arena, NULL);

    static const TypeKind PRIMITIVES[] = {TYPE_I32, TYPE_F32, TYPE_BOOL, TYPE_U8, TYPE_USIZE, TYPE_STR};

    for (size_t i = 0; i < sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]); i++) {
        const Type *type = type_registry_get_primitive(types, PRIMITIVES[i]);

        /* A scope keys its symbols on a mutable name, though binding one only ever hashes it. */
        scope_bind_type(scope, (String *)type_name_of(type), type);
    }

    return scope;
}

Symbol *scope_lookup(Scope *scope, String *name) {
    for (Scope *s = scope; s; s = s->parent) {
        Symbol **found = symbol_table_lookup(s->symbols, name);

        if (found) {
            return *found;
        }
    }

    return NULL;
}

Symbol *scope_lookup_local(Scope *scope, String *name) {
    Symbol **found = symbol_table_lookup(scope->symbols, name);

    return found ? *found : NULL;
}

const Type *symbol_type(TypeRegistry *registry, const Symbol *symbol) {
    if (!symbol) {
        return NULL;
    }

    switch (symbol->kind) {
    case SYMBOL_TYPE:
    case SYMBOL_TYPE_ARG:
        return symbol->type_arg.kind == TYPE_ARG_TYPE ? symbol->type_arg.type : NULL;

    /* A generic names a type only once its arguments are given, but one taking none names itself. */
    case SYMBOL_TYPE_DECL:
        return symbol->type_decl->param_count == 0 ? type_registry_apply(registry, symbol->type_decl, NULL, 0)
                                                   : NULL;

    default:
        return NULL;
    }
}

const Type *scope_type_lookup(TypeRegistry *registry, Scope *scope, String *name) {
    return scope ? symbol_type(registry, scope_lookup(scope, name)) : NULL;
}

Symbol *scope_type_lookup_declaring(Scope *scope, String *name) {
    for (Scope *s = scope; s; s = s->parent) {
        Symbol *found = scope_lookup_local(s, name);

        if (found) {
            return found;
        }

        /* A module's own declarations, so the walk stops where this module does. */
        if (s->kind == SCOPE_MODULE) {
            return NULL;
        }
    }

    return NULL;
}

Symbol *scope_lookup_declaring(Scope *scope, String *name) {
    for (Scope *s = scope;; s = s->parent) {
        Symbol *found = scope_lookup_local(s, name);

        if (found) {
            return found;
        }

        /* A module's symbols and its files', which are what a redeclaration would collide with. */
        if (s->kind != SCOPE_FILE || !s->parent) {
            return NULL;
        }
    }
}

void scope_withdraw(Scope *scope, String *name) { symbol_table_delete(scope->symbols, name); }

/* Binds a symbol of this shape, or answers false where the scope already binds the name. */
static bool scope_bind(Scope *scope, String *name, Symbol shape) {
    if (symbol_table_lookup(scope->symbols, name)) {
        return false;
    }

    Symbol *symbol = arena_alloc(scope->arena, sizeof(Symbol));

    *symbol = shape;

    return symbol_table_insert(scope->symbols, name, symbol) != NULL;
}

bool scope_bind_type(Scope *scope, String *name, const Type *type) {
    assert(type_names_itself(type) && "a nominal name binds to what it declares");

    return scope_bind(scope, name,
                      (Symbol){.kind = SYMBOL_TYPE, .type_arg = {.kind = TYPE_ARG_TYPE, .type = type}});
}

bool scope_bind_type_arg(Scope *scope, String *name, TypeArg arg) {
    return scope_bind(scope, name, (Symbol){.kind = SYMBOL_TYPE_ARG, .type_arg = arg});
}

bool scope_bind_type_decl(Scope *scope, String *name, const TypeDecl *decl) {
    return scope_bind(scope, name, (Symbol){.kind = SYMBOL_TYPE_DECL, .type_decl = decl});
}

bool scope_bind_interface(Scope *scope, String *name, InterfaceDecl *interface) {
    return scope_bind(scope, name, (Symbol){.kind = SYMBOL_INTERFACE, .interface = interface});
}

bool scope_bind_module(Scope *scope, String *name, Module *module) {
    return scope_bind(scope, name, (Symbol){.kind = SYMBOL_MODULE, .module = module});
}

Symbol *scope_decl_var(Scope *scope, String *name, const Type *type) {
    return scope_decl_var_against(scope, scope, name, type);
}

Symbol *scope_decl_var_against(Scope *scope, Scope *against, String *name, const Type *type) {
    if (scope_lookup_declaring(against, name)) {
        return NULL;
    }

    Symbol *symbol = arena_alloc(scope->arena, sizeof(Symbol));

    symbol->kind = SYMBOL_VAR;
    symbol->pinned = false;
    symbol->var.type = type;

    Symbol **declared = symbol_table_insert(scope->symbols, name, symbol);

    return declared ? *declared : NULL;
}

Symbol *scope_decl_func(Scope *scope, String *name, const Type *return_type) {
    return scope_decl_func_against(scope, scope, name, return_type);
}

Symbol *scope_decl_func_against(Scope *scope, Scope *against, String *name, const Type *return_type) {
    if (scope_lookup_declaring(against, name)) {
        return NULL;
    }

    Symbol *symbol = arena_alloc(scope->arena, sizeof(Symbol));

    symbol->kind = SYMBOL_FUNC;
    symbol->pinned = false;

    FuncDecl *func_decl = arena_alloc(scope->arena, sizeof(FuncDecl));

    *func_decl = (FuncDecl){
        .id = {.name = name}, .linkage = LINKAGE_INTERNAL, .signature = {.return_type = return_type}};

    symbol->func = arena_alloc(scope->arena, sizeof(Function));

    *symbol->func = (Function){
        .decl = func_decl,
        .signature = func_decl->signature,
    };

    Symbol **declared = symbol_table_insert(scope->symbols, name, symbol);

    return declared ? *declared : NULL;
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

InstanceId instance_id_of_function(const Function *function) {
    if (!function || !function->decl) {
        return (InstanceId){0};
    }

    return instance_id_of(function->decl->id, function->type_args, function->type_arg_count);
}
