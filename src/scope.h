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

typedef struct Module {
    String *name;

    Scope *scope;
} Module;

#define module_map_hash(key) (size_t)key
#define module_map_key_equals(key, other) key == other

GAB_HASH_MAP(ModuleMap, module_map, String *, Module *)

#define SYMBOL_TABLE_INITIAL_CAPACITY 8

typedef enum {
    SYMBOL_VAR,
    SYMBOL_FUNC,

    SYMBOL_MODULE,

    SYMBOL_TYPE,

    SYMBOL_CONST,

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

#define GAB_LOCATION_TYPE "Location"

#define GAB_STD_MODULE "std"

Scope *global_scope_create(Arena *arena, TypeRegistry *types);

Scope *scope_create(Arena *arena, Scope *parent);

void scope_init_kind(Scope *scope, Arena *arena, Scope *parent, ScopeKind kind);
Scope *scope_create_kind(Arena *arena, Scope *parent, ScopeKind kind);

Symbol *scope_lookup(Scope *scope, String *name);

Symbol *scope_lookup_local(Scope *scope, String *name);

const Type *symbol_type(TypeRegistry *registry, const Symbol *symbol);

const Type *scope_type_lookup(TypeRegistry *registry, Scope *scope, String *name);

Symbol *scope_type_lookup_declaring(Scope *scope, String *name);

Symbol *scope_lookup_declaring(Scope *scope, String *name);

bool scope_bind_type(Scope *scope, String *name, const Type *type);

bool scope_bind_type_param(Scope *scope, String *name, const Type *type);
bool scope_bind_const(Scope *scope, String *name, TypeConst constant);
bool scope_bind_type_decl(Scope *scope, String *name, const TypeDecl *decl);
bool scope_bind_interface(Scope *scope, String *name, InterfaceDecl *interface);
bool scope_bind_module(Scope *scope, String *name, Module *module);

Symbol *scope_bind_func_against(Scope *scope, Scope *against, String *name, Function *function);

Symbol *scope_decl_var_against(Scope *scope, Scope *against, String *name, const Type *type);

Symbol *scope_decl_var(Scope *scope, String *name, const Type *type);

#endif
