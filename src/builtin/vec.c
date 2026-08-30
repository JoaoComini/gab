#include "builtin/builtin.h"

#include "allocator.h"
#include "arena.h"
#include "object.h"
#include "type/type_registry.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <stdint.h>
#include <string.h>

static const TypeDef *vec_declare_type(VM *vm) {
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    const TypeFieldSpec fields[] = {
        {
            .name = string_from_cstr(&vm->env.strings, "data"),
            .type = type_registry_block_of(registry, type_registry_param(registry, 0)),
        },
    };

    const BuiltinTypeSpec spec = {
        .name = "Vec",
        .param_count = 1,
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(*fields),
    };

    return builtin_declare(vm, &spec);
}

typedef struct {
    GabBlockValue block;
} VecHeader;

static VecHeader vec_load(Args *args) {
    VecHeader vec;
    memcpy(&vec, args_pointer(args, 0), sizeof(vec));

    return vec;
}

static void vec_store(Args *args, const VecHeader *vec) { memcpy(args_pointer(args, 0), vec, sizeof(*vec)); }

static size_t vec_stride(Args *args) {
    const Type *receiver = NULL;
    args_address(args, 0, &receiver);

    TypeRegistry *registry = args->vm->env.global_scope.type_registry;

    return type_registry_size_of(
        registry, type_pointee(type_registry_fields_of(registry, type_pointee(receiver))->fields[0].type));
}

static void vec_push(Args *args) {
    VecHeader vec = vec_load(args);
    size_t stride = vec_stride(args);

    if (!block_reserve(DEFAULT_ALLOCATOR, &vec.block, 1, stride)) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory growing a vector");
        return;
    }

    const Type *element = NULL;
    const uint8_t *value = args_address(args, 1, &element);

    memcpy((char *)vec.block.data + (size_t)vec.block.length * stride, value, stride);

    vec.block.length++;

    vec_store(args, &vec);
}

static void vec_at(Args *args) {
    VecHeader vec = vec_load(args);
    int32_t index = args_int(args, 1);

    if (index < 0 || index >= vec.block.length) {
        vm_fail(args->vm, VM_RUN_ERR_EXTERN, "vector index is out of range");
        return;
    }

    size_t stride = vec_stride(args);

    args_return_struct(args, (const char *)vec.block.data + (size_t)index * stride, stride);
}

static void vec_len(Args *args) { args_return_int(args, vec_load(args).block.length); }

static void vec_new(Args *args) {
    int32_t count = args_int(args, 0);

    if (count < 0) {
        vm_fail(args->vm, VM_RUN_ERR_EXTERN, "a vector cannot reserve a negative count");
        return;
    }

    TypeRegistry *registry = args->vm->env.global_scope.type_registry;

    size_t stride = type_registry_size_of(
        registry, type_pointee(type_registry_fields_of(registry, args_return_type(args))->fields[0].type));

    VecHeader vec = {0};

    if (count > 0 && !block_reserve(DEFAULT_ALLOCATOR, &vec.block, count, stride)) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory reserving a vector");
        return;
    }

    args_return_struct(args, &vec, sizeof vec);
}

void builtin_register_vec(VM *vm) {
    const TypeDef *vec_def = vec_declare_type(vm);

    TypeRegistry *registry = vm->env.global_scope.type_registry;

    const Type *element = type_registry_param(registry, 0);
    const Type *self = type_registry_apply(registry, vec_def, &element, 1);
    const Type *receiver = type_registry_ref_to(registry, self);

    const Type *an_int = type_registry_get_primitive(registry, TYPE_INT);

    const Type *const push_params[] = {element};
    const Type *const at_params[] = {an_int};

    builtin_register_method(vm, self, receiver, "push", vec_push, NULL, push_params, 1);
    builtin_register_method(vm, self, receiver, "at", vec_at, element, at_params, 1);
    builtin_register_method(vm, self, receiver, "len", vec_len, an_int, NULL, 0);

    const Type *const new_params[] = {an_int};

    builtin_register_static(vm, self, "new", vec_new, self, new_params, 1);
}
