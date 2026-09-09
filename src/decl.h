#ifndef GAB_DECL_H
#define GAB_DECL_H

#include "string/string.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ASTStmt ASTStmt;

typedef struct Function Function;

/* Where a function's definition is, which is what its symbol must name. */
typedef enum {
    LINKAGE_INTERNAL,

    /* Another Gab unit, whose module the symbol keeps rather than taking the referring one's. */
    LINKAGE_GAB,

    /* A C body, whose symbol is what the declaration spells. */
    LINKAGE_C,
} Linkage;

/* What qualifies a function beyond where it is defined. These compose, and none excludes another. */
typedef enum {
    FUNC_MOD_NONE = 0,

    /* The compiler lowers the call itself, so nothing is bound and no body is written. */
    FUNC_MOD_INTRINSIC = 1 << 0,

    /* '__line__' in the body is the line of the call that reached it. */
    FUNC_MOD_CALLER = 1 << 1,
} FuncModifier;

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

typedef struct InterfaceDecl {
    DeclId id;

    /* Resolved once, with 'Self' as type parameter 0 and the interface's own as 1..param_count;
     * an implementor substitutes itself and its arguments for them. */
    Function *const *methods;
    size_t method_count;

    size_t param_count;
} InterfaceDecl;

/* An interface applied to arguments, which is what a bound states and what a conformance answers. */
typedef struct InterfaceRef {
    const InterfaceDecl *interface;

    TypeArg args[GAB_MAX_TYPE_PARAMS];
    size_t arg_count;
} InterfaceRef;

/* What a type parameter was declared against. A type parameter is bounded by an interface it must
 * implement; a value parameter names the type its value has, as 'array<T, N: i32>' does. */
typedef struct TypeParamBound {
    enum {
        BOUND_NONE,

        BOUND_INTERFACE,

        BOUND_VALUE,
    } kind;

    union {
        InterfaceRef interface;

        const Type *value;
    };
} TypeParamBound;

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

    /* What each of them was declared against, by index. */
    const TypeParamBound *type_param_bounds;
} FuncDecl;

typedef struct Function {
    const FuncDecl *decl;

    /* Its declaration's signature with 'type_args' substituted in, held here since every reader wants
     * the concrete form and substituting allocates. */
    FuncSignature signature;

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

#endif
