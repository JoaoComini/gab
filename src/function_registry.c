#include "function_registry.h"

#include "util/hash_map.h"

#include <string.h>

#define FUNCTION_REGISTRY_INITIAL_CAPACITY 8

typedef struct InstanceKey {
    const Function *generic;

    const Type *args[GAB_MAX_TYPE_PARAMS];
    size_t arg_count;
} InstanceKey;

static inline size_t instance_key_hash_of(InstanceKey key) {
    size_t hash = (size_t)key.generic;

    for (size_t i = 0; i < key.arg_count; i++) {
        hash = hash * 31 + (size_t)key.args[i];
    }

    return hash;
}

static inline bool instance_key_equals(InstanceKey key, InstanceKey other) {
    if (key.generic != other.generic || key.arg_count != other.arg_count) {
        return false;
    }

    return memcmp(key.args, other.args, key.arg_count * sizeof(const Type *)) == 0;
}

#define instance_key_hash(key) instance_key_hash_of(key)
#define instance_key_key_equals(key, other) instance_key_equals(key, other)

GAB_HASH_MAP(InstanceTable, instance_key, InstanceKey, Function *)

struct FunctionRegistry {
    Arena *arena;

    TypeRegistry *types;

    InstanceTable *instances;
};

FunctionRegistry *function_registry_create(Arena *arena, TypeRegistry *types) {
    FunctionRegistry *registry = arena_alloc(arena, sizeof(FunctionRegistry));

    registry->arena = arena;
    registry->types = types;
    registry->instances =
        instance_key_create_alloc(arena_allocator(arena), FUNCTION_REGISTRY_INITIAL_CAPACITY);

    return registry;
}

void function_registry_destroy(FunctionRegistry *registry) {
    if (registry) {
        instance_key_destroy(registry->instances);
    }
}

static InstanceKey key_of(const Function *generic, const Type *const *args, size_t arg_count) {
    InstanceKey key = {.generic = generic, .arg_count = arg_count};

    for (size_t i = 0; i < arg_count && i < GAB_MAX_TYPE_PARAMS; i++) {
        key.args[i] = args[i];
    }

    return key;
}

Function *function_registry_specialize(FunctionRegistry *registry, Function *generic, const Type *const *args,
                                       size_t arg_count) {
    InstanceKey key = key_of(generic, args, arg_count);

    Function **cached = instance_key_lookup(registry->instances, key);

    if (cached) {
        return *cached;
    }

    Function *function = arena_alloc(registry->arena, sizeof(Function));

    const Type **params = generic->param_count
                              ? arena_alloc(registry->arena, generic->param_count * sizeof(const Type *))
                              : NULL;

    for (size_t p = 0; p < generic->param_count; p++) {
        params[p] = type_registry_substitute(registry->types, generic->params[p], args, arg_count);
    }

    const Type **owned_args = arena_alloc(registry->arena, arg_count * sizeof(const Type *));

    for (size_t i = 0; i < arg_count; i++) {
        owned_args[i] = args[i];
    }

    *function = (Function){
        .decl = generic->decl,
        .params = params,
        .param_count = generic->param_count,
        .return_type = type_registry_substitute(registry->types, generic->return_type, args, arg_count),
        .type_args = owned_args,
        .type_arg_count = arg_count,
        .func_index = FUNCTION_NO_BODY,
    };

    instance_key_insert(registry->instances, key, function);

    return function;
}
