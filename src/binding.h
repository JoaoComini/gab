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

#define FUNCTION_NO_BODY ((size_t)-1)

typedef enum {
    BINDING_VAR,
    BINDING_FUNC,
} BindingKind;

typedef struct ASTStmt ASTStmt;

typedef struct Function {
    const Type *return_type;

    const Type **params;
    size_t param_count;

    size_t func_index;

    BodyKind body_kind;

    String *name;
    String *module;

    void *body;

    const Type *const *type_args;
    size_t type_arg_count;

    struct ASTStmt *instance;

    /* The parameters a returned borrow may name; unset until the body's flow pass computes it. */
    uint32_t borrowed_params;
    bool borrowed_params_known;
} Function;

static inline bool function_runs_native(const Function *function) { return function->body_kind != BODY_GAB; }

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
#define binding_table_key_dup(key) key
#define binding_table_entry_free(key, value)

GAB_HASH_MAP(BindingTable, binding_table, String *, Binding *);

#endif
