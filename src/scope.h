#ifndef GAB_SCOPE_H
#define GAB_SCOPE_H

#include "arena.h"
#include "string/string.h"
#include "type/type_registry.h"

typedef struct FunctionRegistry FunctionRegistry;

#include <stdbool.h>

typedef struct BindingTable BindingTable;
typedef struct Binding Binding;
typedef struct Scope Scope;

#define module_scope_map_hash(key) (size_t)key
#define module_scope_map_key_equals(key, other) key == other
#define module_scope_map_key_dup(key) key
#define module_scope_map_entry_free(key, value)

GAB_HASH_MAP(ModuleScopeMap, module_scope_map, String *, Scope *)

typedef struct Scope {
    Arena *arena;

    BindingTable *bindings;
    TypeMap *types;

    TypeRegistry *type_registry;

    FunctionRegistry *functions;

    StringPool *strings;

    struct Scope *parent;
    int depth;

    bool declares_module;
} Scope;

Scope *scope_create(Arena *arena, StringPool *strings, Scope *parent);
void scope_init(Scope *scope, Arena *arena, StringPool *strings, Scope *parent);

void scope_init_over(Scope *scope, Arena *arena, StringPool *strings, TypeRegistry *registry);

void scope_init_at_depth(Scope *scope, Arena *arena, StringPool *strings, Scope *parent, int depth);

void scope_init_module(Scope *scope, Arena *arena, StringPool *strings, Scope *parent);

typedef enum {
    RESOLUTION_NONE,

    RESOLUTION_TYPE,

    RESOLUTION_TYPE_DECL,

    RESOLUTION_VALUE,
} ResolutionKind;

typedef struct {
    ResolutionKind kind;

    union {
        const Type *type;
        const TypeDef *def;
        Binding *binding;
    };
} Resolution;

Resolution scope_resolve(Scope *scope, String *name);

const Type *resolution_type(TypeRegistry *registry, Resolution resolution);

Binding *scope_binding_lookup(Scope *scope, String *name);

const Type *scope_type_lookup(Scope *scope, String *name);

TypeBinding *scope_binding_lookup_local(Scope *scope, String *name);

bool scope_declares_type(Scope *scope, String *name);
Binding *scope_binding_lookup_declaring(Scope *scope, String *name);

void scope_withdraw_type(Scope *scope, String *name);

bool scope_bind_type(Scope *scope, String *name, const Type *type);

bool scope_bind_argument(Scope *scope, String *name, const Type *type);

bool scope_bind_decl(Scope *scope, String *name, const TypeDef *def);

void scope_init_staging(Scope *scope, Arena *arena, StringPool *strings, Scope *target);

void scope_merge_staged(Scope *target, Scope *staged);

Binding *scope_decl_var(Scope *scope, String *name, const Type *type);
Binding *scope_decl_func(Scope *scope, String *name, const Type *return_type);

#endif
