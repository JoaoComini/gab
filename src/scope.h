#ifndef GAB_SCOPE_H
#define GAB_SCOPE_H

#include "decl.h"
#include "memory/arena.h"
#include "string/string.h"
#include "type/type_registry.h"

typedef struct FunctionRegistry FunctionRegistry;

#include <stdbool.h>

typedef struct Scope Scope;

typedef struct ASTStmt ASTStmt;

typedef struct {
    /* What the name binds: a type, or the value 'array<T, N>' names for its length. */
    TypeArg arg;

    const TypeDecl *decl;
} TypeBinding;

#define type_map_hash(key) (size_t)key
#define type_map_key_equals(key, other) key == other

GAB_HASH_MAP(TypeMap, type_map, String *, TypeBinding)

#define interface_map_hash(key) (size_t)key
#define interface_map_key_equals(key, other) key == other

GAB_HASH_MAP(InterfaceMap, interface_map, String *, InterfaceDecl *)

#define module_scope_map_hash(key) (size_t)key
#define module_scope_map_key_equals(key, other) key == other

GAB_HASH_MAP(ModuleScopeMap, module_scope_map, String *, Scope *)

#define BINDING_TABLE_INITIAL_CAPACITY 8

typedef enum {
    BINDING_VAR,
    BINDING_FUNC,
} BindingKind;

typedef struct Binding {
    BindingKind kind;

    int scope_depth;

    bool pinned;

    union {
        struct {
            const Type *type;
        } var;

        Function *func;
    };
} Binding;

#define binding_table_hash(key) (size_t)key
#define binding_table_key_equals(key, other) key == other

GAB_HASH_MAP(BindingTable, binding_table, String *, Binding *);

typedef struct Scope {
    Arena *arena;

    BindingTable *bindings;
    TypeMap *types;

    InterfaceMap *interfaces;

    TypeRegistry *type_registry;

    FunctionRegistry *functions;

    StringPool *strings;

    struct Scope *parent;
    int depth;

    bool declares_module;
} Scope;

/* The struct the core declares for a source position, which '@caller()' answers with. */
#define GAB_LOCATION_TYPE "Location"

#define GAB_STD_MODULE "std"

Scope *scope_create(Arena *arena, StringPool *strings, Scope *parent);
void scope_init(Scope *scope, Arena *arena, StringPool *strings, Scope *parent);

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
        /* A name bound as a generic argument, which is a type unless the declaration takes a value. */
        TypeArg arg;
        const TypeDecl *decl;
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

bool scope_bind_argument(Scope *scope, String *name, TypeArg arg);

bool scope_bind_decl(Scope *scope, String *name, const TypeDecl *decl);

bool scope_bind_interface(Scope *scope, String *name, InterfaceDecl *interface);

InterfaceDecl *scope_interface_lookup(Scope *scope, String *name);

void scope_init_staging(Scope *scope, Arena *arena, StringPool *strings, Scope *target);

void scope_merge_staged(Scope *target, Scope *staged);

Binding *scope_decl_var(Scope *scope, String *name, const Type *type);
Binding *scope_decl_func(Scope *scope, String *name, const Type *return_type);

#endif
