#ifndef GAB_SCOPE_H
#define GAB_SCOPE_H

#include "arena.h"
#include "string/string.h"
#include "type_registry.h"

typedef struct SymbolTable SymbolTable;
typedef struct Symbol Symbol;

typedef struct Scope {
    Arena *arena;

    // The two things a name can mean, held the same way and chained the same
    // way. A lookup that misses here continues in 'parent'.
    SymbolTable *symbol_table;
    TypeMap *types;

    // Shared by every scope, never chained: interning, not naming. See
    // type_registry.h.
    TypeRegistry *type_registry;

    StringPool *strings;

    struct Scope *parent;
    int depth;

    // Stamped onto whatever this scope declares. Set on the scopes a compile
    // declares into — the root and the module scopes — and inherited by the
    // block scopes beneath them.
    unsigned int generation;
} Scope;

// Sets the generation stamped onto subsequent declarations. Called once per
// compile on the scope that compile declares into.
void scope_set_generation(Scope *scope, unsigned int generation);

// Initialize a new scope
Scope *scope_create(Arena *arena, StringPool *strings, Scope *parent);
void scope_init(Scope *scope, Arena *arena, StringPool *strings, Scope *parent);

// As scope_init, but with the depth given rather than derived from the parent.
void scope_init_at_depth(Scope *scope, Arena *arena, StringPool *strings, Scope *parent, int depth);

// A module scope: parented to the root for symbol and type fallback, but held
// at depth 0, since its declarations are a unit's top level and not a nested
// block. Depth drives the pointer-lifetime rule, where 0 means 'outlives
// everything' — depth 1 would make '&top_level_var' look like a pointer into a
// block. Unlike a block scope it gets its own type registry, so the types a
// module declares are namespaced by the registry rather than by their name.
void scope_init_module(Scope *scope, Arena *arena, StringPool *strings, Scope *parent);

Symbol *scope_symbol_lookup(Scope *scope, String *name);

// The type this name means here: this scope, then outward. A module's own
// 'Config' therefore shadows a root-level one, and 'int' resolves from
// anywhere because the root scope declares it.
Type *scope_type_lookup(Scope *scope, String *name);

// As scope_type_lookup, but only this scope. For declaration, where shadowing
// an outer name is allowed and redeclaring one of this scope's own is not.
Type *scope_type_lookup_local(Scope *scope, String *name);

// Returns false if this scope already declares the name at the current
// generation — that is, if this same compile declared it. A name an earlier
// compile declared is replaced, since recompiling a unit redeclares its types.
bool scope_decl_type(Scope *scope, String *name, Type *type);

// Whether this scope declares the name at the current generation. Distinct
// from scope_type_lookup_local, which finds an earlier compile's declaration
// too and so cannot tell a duplicate from something a reload may replace.
bool scope_declares_type_now(Scope *scope, String *name);
Symbol *scope_decl_var(Scope *scope, String *name, Type *type);
Symbol *scope_decl_func(Scope *scope, String *name, Type *return_type);

#endif
