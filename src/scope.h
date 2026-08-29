#ifndef GAB_SCOPE_H
#define GAB_SCOPE_H

#include "arena.h"
#include "string/string.h"
#include "type/type_registry.h"

#include <stdbool.h>

typedef struct SymbolTable SymbolTable;
typedef struct Symbol Symbol;
typedef struct Scope Scope;

// Module name to the scope holding its declarations. Keyed on the interned
// name, so identity comparison is enough.
#define module_scope_map_hash(key) (size_t)key
#define module_scope_map_key_equals(key, other) key == other
#define module_scope_map_key_dup(key) key
#define module_scope_map_entry_free(key, value)

GAB_HASH_MAP(ModuleScopeMap, module_scope_map, String *, Scope *)

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

    // Where a declaration's search for a clashing name stops. Set on a module
    // scope and on the staging scope standing in for one, so the two are
    // searched together and the root beneath them is not: a unit may shadow a
    // builtin, but may not redeclare what its own module already has.
    bool declares_module;
} Scope;

// Initialize a new scope
Scope *scope_create(Arena *arena, StringPool *strings, Scope *parent);
void scope_init(Scope *scope, Arena *arena, StringPool *strings, Scope *parent);

// As scope_init, over a registry built beforehand. What a VM uses: a standard
// library registers its types with the registry first, and this names them
// alongside the primitives -- a scope names what is registered when it is
// built, so the order is the whole of why this exists.
void scope_init_over(Scope *scope, Arena *arena, StringPool *strings, TypeRegistry *registry);

// As scope_init, but with the depth given rather than derived from the parent.
void scope_init_at_depth(Scope *scope, Arena *arena, StringPool *strings, Scope *parent, int depth);

// A module scope: parented to the root for symbol and type fallback, but held
// at depth 0, since its declarations are a unit's top level and not a nested
// block. Depth drives the pointer-lifetime rule, where 0 means 'outlives
// everything' — depth 1 would make 'ref top_level_var' look like a pointer into a
// block. Unlike a block scope it gets its own type registry, so the types a
// module declares are namespaced by the registry rather than by their name.
void scope_init_module(Scope *scope, Arena *arena, StringPool *strings, Scope *parent);

// What a name means here, whichever namespace it was found in. One answer type
// rather than a lookup per kind: a mention that could be several things --
// 'Foo(x)' is a conversion where 'Foo' is a type and a call where it is a
// function -- decides on the kind rather than by asking each namespace in turn
// and reading a NULL as the answer.
//
// Rust's Res, and split the same way it is: a primitive was declared by
// nothing, so it carries a type and no declaration.
typedef enum {
    RESOLUTION_NONE,

    // 'int', 'float', 'bool', 'str'. Named by the root scope and declared by
    // nothing, so there is no declaration to instantiate -- the type is what
    // the name means.
    RESOLUTION_PRIMITIVE,

    // Every nominal name, whatever arity: 'Config', 'Vec', 'Array'. What it
    // takes is the declaration's business rather than the resolution's, so a
    // bare mention of one taking parameters is an arity error at the mention
    // and not a second kind here.
    RESOLUTION_TYPE_DECL,

    // A local or a function.
    RESOLUTION_VALUE,
} ResolutionKind;

typedef struct {
    ResolutionKind kind;

    union {
        const Type *primitive;
        const TypeDef *def;
        Symbol *symbol;
    };
} Resolution;

// What this name means: this scope, then outward. The types are consulted
// before the values, which is what makes a conversion win over a call of the
// same name.
Resolution scope_resolve(Scope *scope, String *name);

// The type a resolution stands for, or NULL where it stands for none: a
// declaration still owed arguments, a value, or nothing at all. The registry is
// what turns a declaration taking none into its one instantiation.
const Type *resolution_type(TypeRegistry *registry, Resolution resolution);

Symbol *scope_symbol_lookup(Scope *scope, String *name);

// The type this name means here: this scope, then outward. A module's own
// 'Config' therefore shadows an outer one, and 'int' resolves from
// anywhere because the root scope declares it.
const Type *scope_type_lookup(Scope *scope, String *name);

// What this scope itself binds the name to, declaration included. For a site
// asking which declaration a name reaches rather than what type it names.
TypeBinding *scope_binding_lookup_local(Scope *scope, String *name);

// What a declaration of this name would clash with. Both search this scope and,
// through a staging scope, the module scope it stands in for -- so a name an
// earlier unit declared is an error where this unit writes it, rather than one
// reported without a span after the whole unit has compiled.
//
// They differ at the root. A type also clashes with a builtin, which no syntax
// can name past a shadow; a symbol does not, so a local may be called 'int'.
const Type *scope_type_lookup_declaring(Scope *scope, String *name);
Symbol *scope_symbol_lookup_declaring(Scope *scope, String *name);

// Removes a name this scope declared. Exists for one case: a struct's name is
// bound before its fields resolve, so that any field in the unit may name it,
// and one whose layout then fails has to be taken back out — it has no width,
// so nothing may use it as a type.
void scope_withdraw_type(Scope *scope, String *name);

// Binds a name to a type that names itself -- a primitive, or a declaration's
// own parameter -- which is instantiated from no declaration and so has none to
// be bound to.
//
// Returns false if this scope already declares the name. Declaring a name is a
// one-time act: a second declaration of it, in this unit or a later one, is a
// collision rather than a replacement.
bool scope_decl_type(Scope *scope, String *name, const Type *type);

// Binds a name to what it declares, which every nominal name is bound to
// whatever its arity: the type one taking no parameters stands for is that
// declaration applied to none, and the registry is what holds it.
bool scope_decl_type_def(Scope *scope, String *name, const TypeDef *def);

// A scope a compile declares into instead of its real target, so a compile that
// fails declares nothing. Parented to the target, so lookups still reach what is
// already declared.
void scope_init_staging(Scope *scope, Arena *arena, StringPool *strings, Scope *target);

// Moves what a staging scope declared into the scope it stood in for. Cannot
// collide: a name already in the module was refused where it was declared.
void scope_merge_staged(Scope *target, Scope *staged);

Symbol *scope_decl_var(Scope *scope, String *name, const Type *type);
Symbol *scope_decl_func(Scope *scope, String *name, const Type *return_type);

#endif
