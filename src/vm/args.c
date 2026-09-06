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

uint8_t *args_address(Args *args, int index) {
    assert(args && "a C body was called without a frame");

    const Function *function = args->function;

    assert(index >= 0 && (size_t)index < function->param_count &&
           "a C body read a parameter its declaration does not have");
    (void)function;

    return args->vm->stack + args->base + args->param_offsets[index];
}

const Type *args_param_type(Args *args, int index) {
    const Function *function = args->function;

    return (index >= 0 && (size_t)index < function->param_count) ? function->params[index] : NULL;
}

uint8_t *args_return_address(Args *args) {
    assert(args && "a C body returned without a frame");

    return args->vm->stack + args->base;
}

static uint8_t *args_address_of_kind(Args *args, int index, TypeKind kind) {
    const Type *type = args_param_type(args, index);
    uint8_t *at = args_address(args, index);

    assert(type && type_kind(type) == kind && "a C body read a parameter as a type it was not declared");
    (void)kind;
    (void)type;

    return at;
}

int32_t args_int(Args *args, int index) {
    int32_t value;
    memcpy(&value, args_address_of_kind(args, index, TYPE_INT), sizeof(value));

    return value;
}

float args_float(Args *args, int index) {
    float value;
    memcpy(&value, args_address_of_kind(args, index, TYPE_FLOAT), sizeof(value));

    return value;
}

bool args_bool(Args *args, int index) {
    int32_t value;
    memcpy(&value, args_address_of_kind(args, index, TYPE_BOOL), sizeof(value));

    return value != 0;
}

StrRef args_string(Args *args, int index) {
    const Type *type = args_param_type(args, index);
    uint8_t *at = args_address(args, index);

    assert(type_is_str_ref(type) &&
           "a C body read a parameter as a borrowed string when it was not declared one");
    (void)type;

    StrRef value;
    memcpy(&value, at, sizeof(value));

    return value;
}

ArrayValue args_array(Args *args, int index) {
    ArrayValue value;
    memcpy(&value, args_address_of_kind(args, index, TYPE_ARRAY), sizeof(value));

    return value;
}

void *args_pointer(Args *args, int index) {
    const Type *type = args_param_type(args, index);
    uint8_t *at = args_address(args, index);

    assert(type && type_is_indirect(type) && "a C body read a parameter as a type it was not declared");
    (void)type;

    void *pointer;
    memcpy(&pointer, at, sizeof(pointer));

    return pointer;
}

void args_struct(Args *args, int index, void *out, size_t size) {
    const Type *type = args_param_type(args, index);
    const uint8_t *at = args_address(args, index);

    assert(out && "a C body read a struct argument into nothing");
    assert(type && type_registry_size_of(args_registry(args), type) == size &&
           "a struct argument was read at a size its type does not have");
    (void)size;
    (void)type;

    memcpy(out, at, size);
}

void args_return_int(Args *args, int32_t value) { memcpy(args_return_address(args), &value, sizeof(value)); }

void args_return_float(Args *args, float value) { memcpy(args_return_address(args), &value, sizeof(value)); }

void args_return_bool(Args *args, bool value) {
    int32_t widened = value ? 1 : 0;

    memcpy(args_return_address(args), &widened, sizeof(widened));
}

void args_return_pointer(Args *args, void *pointer) {
    memcpy(args_return_address(args), &pointer, sizeof(pointer));
}

const Type *args_return_type(Args *args) { return args->function->return_type; }

void args_return_struct(Args *args, const void *data, size_t size) {
    assert(data && "a C body returned a struct from nothing");

    const Type *return_type = args->function->return_type;

    assert(return_type && type_registry_size_of(args_registry(args), return_type) == size &&
           "a struct was returned at a size the declared return type does not have");
    (void)return_type;
    (void)size;

    memcpy(args_return_address(args), data, size);
}
