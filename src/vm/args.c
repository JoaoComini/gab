#include "vm/args.h"

#include "binding.h"
#include "object.h"
#include "type/type_registry.h"
#include "vm/interp.h"
#include "vm/opcode.h"

#include <assert.h>
#include <string.h>

unsigned int args_type_slots(TypeRegistry *registry, const Type *type) {
    if (!type) {
        return 1;
    }

    return (unsigned int)((type_registry_size_of(registry, type) + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);
}

static TypeRegistry *args_registry(Args *args) { return args->vm->env.global_scope.type_registry; }

uint8_t *args_address(Args *args, int index, const Type **out_type) {
    assert(args && "a C body was called without a frame");

    const Function *function = args->function;

    assert(index >= 0 && (size_t)index < function->param_count &&
           "a C body read a parameter its declaration does not have");

    unsigned int slot = 1;

    for (int i = 0; i < index; i++) {
        slot += args_type_slots(args_registry(args), function->params[i]);
    }

    if (out_type) {
        *out_type = function->params[index];
    }

    return args->vm->stack + args->base + (size_t)slot * VM_SLOT_SIZE;
}

const Type *args_param_type(Args *args, int index) {
    const Function *function = args->function;

    return (index >= 0 && (size_t)index < function->param_count) ? function->params[index] : NULL;
}

uint8_t *args_return_address(Args *args) {
    assert(args && "a C body returned without a frame");

    return args->vm->stack + args->base;
}

void *args_box_return(Args *args) {
    const Type *return_type = args->function->return_type;

    if (!return_type || !type_owns_through_an_address(return_type)) {
        vm_fail(args->vm, VM_RUN_ERR_EXTERN, "a C body boxed a value its declaration does not return");
        return NULL;
    }

    TypeRegistry *registry = args_registry(args);
    const Type *pointee = type_pointee(return_type);

    void *object = object_alloc(&DEFAULT_ALLOCATOR, type_registry_size_of(registry, pointee),
                                type_registry_drop_of(registry, pointee));

    if (!object) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory boxing a return value");
        return NULL;
    }

    return object;
}

bool args_string_copy(Args *args, const char *data, int32_t length, StringValue *out) {
    int32_t capacity = length == 0 ? 1 : length;

    char *characters = DEFAULT_ALLOCATOR.alloc(DEFAULT_ALLOCATOR.ctx, (size_t)capacity);

    if (!characters) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory copying a string");
        return false;
    }

    if (length) {
        memcpy(characters, data, (size_t)length);
    }

    *out = (StringValue){.block = {.data = characters, .capacity = capacity, .length = length}};

    return true;
}
