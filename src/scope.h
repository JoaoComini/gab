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

/* A module as another module sees it: what it is called and what it declares. What compiling it
 * concluded is the compiling module's own business, and does not survive into this. */
typedef struct Module {
    String *name;

    Scope *scope;
} Module;

#define module_map_hash(key) (size_t)key
#define module_map_key_equals(key, other) key == other

GAB_HASH_MAP(ModuleMap, module_map, String *, Module *)

#define BINDING_TABLE_INITIAL_CAPACITY 8

typedef enum {
    BINDING_VAR,
    BINDING_FUNC,

    /* What an import binds, so naming the module is the same lookup as naming anything else. */
    BINDING_MODULE,
} BindingKind;

typedef struct Binding {
    BindingKind kind;

    bool pinned;

    union {
        struct {
            const Type *type;
        } var;

        Function *func;

        Module *module;
    };
} Binding;

#define binding_table_hash(key) (size_t)key
#define binding_table_key_equals(key, other) key == other

GAB_HASH_MAP(BindingTable, binding_table, String *, Binding *);

/* What a scope stands for, which decides how far a lookup walks and what may be declared in it.
 * Global holds the primitives, and every module hangs off it: Global -> Module -> File -> Local. */
typedef enum {
    SCOPE_GLOBAL,
    SCOPE_MODULE,
    SCOPE_FILE,
    SCOPE_LOCAL,
} ScopeKind;

typedef struct Scope {
    Arena *arena;

    BindingTable *bindings;
    TypeMap *types;

    InterfaceMap *interfaces;

    struct Scope *parent;

    ScopeKind kind;
} Scope;

/* The struct the core declares for a source position, which '@caller()' answers with. */
#define GAB_LOCATION_TYPE "Location"

#define GAB_STD_MODULE "std"

/* The scope every module hangs off, holding the names the language predeclares. Built once, before
 * any source is read: what the primitives are called is not something a module states. */
Scope *global_scope_create(Arena *arena, TypeRegistry *types);

/* A local scope under 'parent'. */
Scope *scope_create(Arena *arena, Scope *parent);

/* A scope of a given kind, which a lookup treats by what it stands for rather than by how deep it is. */
void scope_init_kind(Scope *scope, Arena *arena, Scope *parent, ScopeKind kind);
Scope *scope_create_kind(Arena *arena, Scope *parent, ScopeKind kind);

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

const Type *scope_type_lookup(TypeRegistry *registry, Scope *scope, String *name);

/* A type this module declares, which is what an 'impl' may name: a type reached through an import
 * belongs to the module that declared it. */
TypeBinding *scope_type_lookup_declaring(Scope *scope, String *name);

bool scope_declares_type(Scope *scope, String *name);
Binding *scope_binding_lookup_declaring(Scope *scope, String *name);

void scope_withdraw_type(Scope *scope, String *name);

bool scope_bind_type(Scope *scope, String *name, const Type *type);

bool scope_bind_argument(Scope *scope, String *name, TypeArg arg);

bool scope_bind_decl(Scope *scope, String *name, const TypeDecl *decl);

bool scope_bind_interface(Scope *scope, String *name, InterfaceDecl *interface);

InterfaceDecl *scope_interface_lookup(Scope *scope, String *name);

/* Declares into 'scope', rejecting a name 'against' already binds: a module's declaration is checked
 * from the file that writes it, so it collides with what that file imports as well. */
Binding *scope_decl_var_against(Scope *scope, Scope *against, String *name, const Type *type);
Binding *scope_decl_func_against(Scope *scope, Scope *against, String *name, const Type *return_type);

Binding *scope_decl_var(Scope *scope, String *name, const Type *type);
Binding *scope_decl_func(Scope *scope, String *name, const Type *return_type);

/* Binds 'module' under the name this file imports it as; false where the name is already taken. */
bool scope_bind_module(Scope *scope, String *name, Module *module);

#endif
