#include "function_registry.h"

#include "util/hash_map.h"

#include <assert.h>

#include <string.h>

#define FUNCTION_REGISTRY_INITIAL_CAPACITY 8

#define instance_key_hash(key) instance_id_hash(key)
#define instance_key_key_equals(key, other) instance_id_equals(key, other)

GAB_HASH_MAP(InstanceTable, instance_key, InstanceId, Function *)

#define owned_key_hash(key) ((decl_id_hash((key).owner) * 31) ^ (size_t)(key).name)
#define owned_key_key_equals(key, other)                                                                     \
    (decl_id_equals((key).owner, (other).owner) && (key).name == (other).name)

GAB_HASH_MAP(OwnedTable, owned_key, TypeMemberKey, Function *)

struct FunctionRegistry {
    Arena *arena;

    TypeRegistry *types;

    InstanceTable *instances;

    OwnedTable *owned;
};

FunctionRegistry *function_registry_create(Arena *arena, TypeRegistry *types) {
    FunctionRegistry *registry = arena_alloc(arena, sizeof(FunctionRegistry));

    registry->arena = arena;
    registry->types = types;
    registry->instances =
        instance_key_create_alloc(arena_allocator(arena), FUNCTION_REGISTRY_INITIAL_CAPACITY);
    registry->owned = owned_key_create_alloc(arena_allocator(arena), FUNCTION_REGISTRY_INITIAL_CAPACITY);

    return registry;
}

void function_registry_destroy(FunctionRegistry *registry) {
    if (registry) {
        instance_key_destroy(registry->instances);
        owned_key_destroy(registry->owned);
    }
}

bool function_registry_declare_owned(FunctionRegistry *registry, const Type *type, Function *function) {
    assert(type && function && function->decl && function->decl->id.name &&
           "a function a type owns has a name and a signature");

    TypeMemberKey key = type_member_key_of(type, function->decl->id.name);

    if (owned_key_lookup(registry->owned, key)) {
        return false;
    }

    owned_key_insert(registry->owned, key, function);

    return true;
}

Function *function_registry_find_owned(FunctionRegistry *registry, const Type *type, const String *name) {
    if (!type) {
        return NULL;
    }

    Function **declared = owned_key_lookup(registry->owned, type_member_key_of(type, name));

    return declared ? *declared : NULL;
}

/* A signature mentioning no type parameter is one function for every instantiation of its owner. */
bool function_registry_owned_is_shared(const Function *declaration, const Type *type) {
    if (type_arg_count(type) == 0) {
        return true;
    }

    for (size_t i = 0; i < declaration->signature.param_count; i++) {
        if (type_has_param(declaration->signature.params[i])) {
            return false;
        }
    }

    return !type_has_param(declaration->signature.return_type);
}

Function *function_registry_destructor(FunctionRegistry *registry, const Type *type) {
    const KnownNames *names = type_registry_names(registry->types);

    if (!type_registry_conforms(registry->types, type, names->destroy_interface, NULL, 0)) {
        return NULL;
    }

    return function_registry_find_owned(registry, type, names->destroy_method);
}

Function *function_registry_owned_for(FunctionRegistry *registry, const Type *type, const String *name) {
    Function *declaration = function_registry_find_owned(registry, type, name);

    if (!declaration || function_registry_owned_is_shared(declaration, type)) {
        return declaration;
    }

    /* A method with parameters of its own is specialized at the call, which knows all of them. */
    if (declaration->decl->type_param_count > type_arg_count(type)) {
        return declaration;
    }

    return function_registry_instance(registry, declaration->decl, type_args(type), type_arg_count(type));
}

Function *function_registry_instance(FunctionRegistry *registry, const FuncDecl *decl, const TypeArg *args,
                                     size_t arg_count) {
    InstanceId key = instance_id_of(decl->id, args, arg_count);

    Function **cached = instance_key_lookup(registry->instances, key);

    if (cached) {
        return *cached;
    }

    Function *function = arena_alloc(registry->arena, sizeof(Function));

    TypeArg *owned_args = arena_alloc(registry->arena, arg_count * sizeof(TypeArg));

    for (size_t i = 0; i < arg_count; i++) {
        owned_args[i] = args[i];
    }

    *function = (Function){
        .decl = decl,
        .signature =
            func_signature_instantiate(registry->types, registry->arena, &decl->signature, args, arg_count),
        .type_args = owned_args,
        .type_arg_count = arg_count,
    };

    instance_key_insert(registry->instances, key, function);

    return function;
}
