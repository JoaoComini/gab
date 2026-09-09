#ifndef GAB_BINDING_H
#define GAB_BINDING_H

#include "scope.h"
#include "string/string.h"
#include "type/type.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BINDING_TABLE_INITIAL_CAPACITY 8

typedef enum {
    BINDING_VAR,
    BINDING_FUNC,
} BindingKind;

typedef struct ASTStmt ASTStmt;

/* Parameters and result. On a declaration these name its type parameters; on a Function they are what
 * substituting that declaration's arguments into them produced. */
typedef struct FuncSignature {
    const Type *return_type;

    const Type **params;
    size_t param_count;
} FuncSignature;

/* Substitutes 'args' for the type parameters a signature names, yielding the specialized one. */
FuncSignature func_signature_instantiate(TypeRegistry *registry, Arena *arena, const FuncSignature *generic,
                                         const TypeArg *args, size_t arg_count);

/* What a record stands for: its declaration, with the arguments it was specialized on. */
InstanceId instance_id_of_function(const Function *function);

#define GAB_INTRINSIC_COUNT 6

/* The owner and name of a call that stands for instructions rather than a body, which IR lowering expands. */
typedef struct IntrinsicLowering {
    const String *owner;
    const String *name;
} IntrinsicLowering;

typedef struct FuncDecl {
    /* What names this declaration: the name a lookup and a diagnostic use, and what a symbol renders
     * from together with the module and owner qualifying it. */
    DeclId id;

    Linkage linkage;

    /* A set of FuncModifier. */
    unsigned modifiers;

    /* The type a 'caller' function's hidden parameter has, which only the core can name. */
    const Type *location_type;

    /* Written with this declaration's type parameters unsubstituted, so an instance substitutes into it. */
    FuncSignature signature;

    /* How many arguments this declaration is generic over, whether they came from an owner or itself. */
    size_t type_param_count;

    /* The bound on each of them, by index; its interface is null where the parameter is unbounded. */
    const InterfaceRef *type_param_bounds;
} FuncDecl;

typedef struct Function {
    const FuncDecl *decl;

    /* Its declaration's signature with 'type_args' substituted in, held here since every reader wants
     * the concrete form and substituting allocates. Unnamed so 'f->params' still reaches it. */
    union {
        FuncSignature signature;

        struct {
            const Type *return_type;

            const Type **params;
            size_t param_count;
        };
    };

    /* What this instance was given, one per type parameter of its declaration. */
    const TypeArg *type_args;
    size_t type_arg_count;

    /* Set on a method standing on a bounded parameter, which substitution resolves to the real one. */
    const Type *bound_self;

    /* The parameters a returned borrow may name; unset until the body's flow pass computes it. */
    uint32_t borrowed_params;
    bool borrowed_params_known;
} Function;

/* True where no Gab body is lowered here: the definition is elsewhere, or the compiler expands it. */
static inline bool function_runs_native(const Function *function) {
    return function->decl->linkage != LINKAGE_INTERNAL ||
           (function->decl->modifiers & FUNC_MOD_INTRINSIC) != 0;
}

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

#endif
