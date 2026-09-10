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
typedef struct FuncDecl FuncDecl;

typedef enum {
    LINKAGE_INTERNAL,

    LINKAGE_GAB,

    LINKAGE_C,
} Linkage;

typedef enum {
    FUNC_MOD_NONE = 0,

    FUNC_MOD_INTRINSIC = 1 << 0,

    FUNC_MOD_CALLER = 1 << 1,
} FuncModifier;

typedef struct FuncSignature {
    const Type *return_type;

    const Type **params;
    size_t param_count;
} FuncSignature;

FuncSignature func_signature_instantiate(TypeRegistry *registry, Arena *arena, const FuncSignature *generic,
                                         const TypeArg *args, size_t arg_count);

InstanceId instance_id_of_function(const Function *function);

#define GAB_INTRINSIC_COUNT 6

typedef struct IntrinsicLowering {
    const String *owner;
    const String *name;
} IntrinsicLowering;

typedef struct InterfaceDecl {
    DeclId id;

    const FuncDecl *const *methods;
    size_t method_count;

    size_t param_count;
} InterfaceDecl;

typedef struct InterfaceRef {
    const InterfaceDecl *interface;

    TypeArg args[GAB_MAX_TYPE_PARAMS];
    size_t arg_count;
} InterfaceRef;

typedef enum {
    BOUND_NONE,

    BOUND_INTERFACE,

    BOUND_VALUE,
} BoundKind;

typedef struct TypeParamBound {
    BoundKind kind;

    union {
        InterfaceRef interface;

        const Type *value;
    };
} TypeParamBound;

typedef struct FuncDecl {
    DeclId id;

    const InterfaceDecl *interface;

    Linkage linkage;

    unsigned modifiers;

    const Type *location_type;

    FuncSignature signature;

    size_t type_param_count;

    const TypeParamBound *type_param_bounds;
} FuncDecl;

typedef struct Function {
    const FuncDecl *decl;

    FuncSignature signature;

    const TypeArg *type_args;
    size_t type_arg_count;

    uint32_t borrowed_params;
    bool borrowed_params_known;
} Function;

static inline bool function_lowers_no_body(const Function *function) {
    return function->decl->linkage != LINKAGE_INTERNAL ||
           (function->decl->modifiers & FUNC_MOD_INTRINSIC) != 0;
}

#endif
