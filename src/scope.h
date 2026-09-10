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

/* A module as another module sees it: what it is called and what it declares. What compiling it
 * concluded is the compiling module's own business, and does not survive into this. */
typedef struct Module {
    String *name;

    Scope *scope;
} Module;

#define module_map_hash(key) (size_t)key
#define module_map_key_equals(key, other) key == other

GAB_HASH_MAP(ModuleMap, module_map, String *, Module *)

#define SYMBOL_TABLE_INITIAL_CAPACITY 8

/* What a name denotes. One table holds them all, so a name means one thing in a scope however it was
 * declared, and what kind of thing is what the lookup answers with. */
typedef enum {
    SYMBOL_VAR,
    SYMBOL_FUNC,

    /* What an import binds, so naming the module is the same lookup as naming anything else. */
    SYMBOL_MODULE,

    /* A type: one this scope declares, or the parameter standing for one inside a generic. */
    SYMBOL_TYPE,

    /* A value a generic takes rather than a type, such as the 'N' in 'array<T, N>'. */
    SYMBOL_CONST,

    /* A generic's declaration, which names a type only once its arguments are given. */
    SYMBOL_TYPE_DECL,

    SYMBOL_INTERFACE,
} SymbolKind;

typedef struct Symbol {
    SymbolKind kind;

    bool pinned;

    union {
        struct {
            const Type *type;
        } var;

        Function *func;

        Module *module;

        const Type *type;

        TypeConst constant;

        const TypeDecl *type_decl;

        InterfaceDecl *interface;
    };
} Symbol;

#define symbol_table_hash(key) (size_t)key
#define symbol_table_key_equals(key, other) key == other

GAB_HASH_MAP(SymbolTable, symbol_table, String *, Symbol *);

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

    SymbolTable *symbols;

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

/* What 'name' denotes here or in an enclosing scope, or null where nothing does. */
Symbol *scope_lookup(Scope *scope, String *name);

/* What it denotes in this scope alone, which is what a redeclaration would collide with. */
Symbol *scope_lookup_local(Scope *scope, String *name);

/* The type a symbol names, or null where it names something that is not one. */
const Type *symbol_type(TypeRegistry *registry, const Symbol *symbol);

/* The type 'name' denotes, resolved through the scope chain. */
const Type *scope_type_lookup(TypeRegistry *registry, Scope *scope, String *name);

/* A type this module declares, which is what an 'impl' may name: a type reached through an import
 * belongs to the module that declared it. */
Symbol *scope_type_lookup_declaring(Scope *scope, String *name);

/* What a declaration would collide with: this scope, and the module's where a file writes into it. */
Symbol *scope_lookup_declaring(Scope *scope, String *name);

void scope_withdraw(Scope *scope, String *name);

/* Each binds 'name' to what it names, and answers false where the scope already binds that name. */
bool scope_bind_type(Scope *scope, String *name, const Type *type);

/* A generic's parameter: the type standing for one inside its body, or the value it takes instead.
 * Unlike a declared type, a parameter names nothing of its own until an argument is given for it. */
bool scope_bind_type_param(Scope *scope, String *name, const Type *type);
bool scope_bind_const(Scope *scope, String *name, TypeConst constant);
bool scope_bind_type_decl(Scope *scope, String *name, const TypeDecl *decl);
bool scope_bind_interface(Scope *scope, String *name, InterfaceDecl *interface);
bool scope_bind_module(Scope *scope, String *name, Module *module);

/* Declares into 'scope', rejecting a name 'against' already binds: a module's declaration is checked
 * from the file that writes it, so it collides with what that file imports as well. */
Symbol *scope_decl_var_against(Scope *scope, Scope *against, String *name, const Type *type);
Symbol *scope_decl_func_against(Scope *scope, Scope *against, String *name, const Type *return_type);

Symbol *scope_decl_var(Scope *scope, String *name, const Type *type);
Symbol *scope_decl_func(Scope *scope, String *name, const Type *return_type);

#endif
