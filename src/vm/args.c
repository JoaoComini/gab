#include "vm/args.h"

#include "object.h"
#include "symbol_table.h"
#include "type_registry.h"
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

// The registry the frame's types were interned in, which is the one every
// layout a C body asks about was computed in. Reached through the VM because a
// call carries no registry of its own: a run needs no types, and this path is
// the one place where a host asks a width of one.
static TypeRegistry *args_registry(Args *args) { return args->vm->env.global_scope.type_registry; }

uint8_t *args_address(Args *args, int index, const Type **out_type) {
    assert(args && "a C body was called without a frame");

    const Symbol *symbol = args->symbol;

    assert(index >= 0 && (size_t)index < symbol->func.param_count &&
           "a C body read a parameter its declaration does not have");

    unsigned int slot = 1;

    for (int i = 0; i < index; i++) {
        slot += args_type_slots(args_registry(args), symbol->func.params[i]);
    }

    if (out_type) {
        *out_type = symbol->func.params[index];
    }

    return args->vm->stack + args->base + (size_t)slot * VM_SLOT_SIZE;
}

uint8_t *args_return_address(Args *args) {
    assert(args && "a C body returned without a frame");

    return args->vm->stack + args->base;
}

// The parameter's address, having asserted that it is the type being read. A
// slot carries no tag, so reading an int as a float reinterprets the bytes
// rather than converting them -- which is why the declaration is what says
// whether a read is the right one.
static uint8_t *args_address_of_kind(Args *args, int index, TypeKind kind) {
    const Type *type = NULL;
    uint8_t *at = args_address(args, index, &type);

    assert(type && type->kind == kind && "a C body read a parameter as a type it was not declared");
    (void)kind;

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

// The characters a borrowed string names, read from the reference itself: a
// 'ref str' is the address and the count side by side, which is the same two
// words an owning header holds and the same two the host reads.
GabStrRef args_string(Args *args, int index) {
    const Type *type = NULL;
    uint8_t *at = args_address(args, index, &type);

    assert(type_is_str_ref(type) &&
           "a C body read a parameter as a borrowed string when it was not declared one");

    // Read as what a reference is, never as the header it may have been lent
    // from: a header carrying more than the reference does still lends exactly
    // these two words.
    GabStrRef value;
    memcpy(&value, at, sizeof(value));

    return value;
}

// The header a pointer parameter names. A 'ref String' is an address like any
// other indirection, so the slots it occupies hold no characters of their own
// and the header is read from where it points.
GabStringValue args_string_at(Args *args, int index) {
    const uint8_t *at = args_pointer(args, index);

    assert(at && "a C body read a string through a pointer holding nothing");

    GabStringValue value;
    memcpy(&value, at, sizeof(value));

    return value;
}

GabArrayValue args_array(Args *args, int index) {
    GabArrayValue value;
    memcpy(&value, args_address_of_kind(args, index, TYPE_ARRAY), sizeof(value));

    return value;
}

void *args_pointer(Args *args, int index) {
    const Type *type = NULL;
    uint8_t *at = args_address(args, index, &type);

    // Either constructor: a C body is handed an address, and whether the script
    // owns what it names is the script's business rather than this read's.
    assert(type && type_is_indirect(type) && "a C body read a parameter as a type it was not declared");

    void *pointer;
    memcpy(&pointer, at, sizeof(pointer));

    return pointer;
}

void args_struct(Args *args, int index, void *out, size_t size) {
    const Type *type = NULL;
    const uint8_t *at = args_address(args, index, &type);

    assert(out && "a C body read a struct argument into nothing");
    assert(type && type_registry_size_of(args_registry(args), type) == size &&
           "a struct argument was read at a size its type does not have");
    (void)size;

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

bool args_return_string_copy(Args *args, const char *data, int32_t length) {
    // A capacity of at least one, so that the empty string still names memory
    // the free path can hand back: a block is freed at its capacity, and a null
    // one would have to be told apart from a real allocation everywhere.
    int32_t capacity = length == 0 ? 1 : length;

    // Typed as characters rather than as a string: the bytes are what is
    // allocated, and they own nothing further. The header naming them is the
    // value below.
    char *characters = DEFAULT_ALLOCATOR.alloc(DEFAULT_ALLOCATOR.ctx, (size_t)capacity);

    if (!characters) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory copying a string");
        return false;
    }

    if (length) {
        memcpy(characters, data, (size_t)length);
    }

    GabStringValue value = {.block = {.data = characters, .capacity = capacity, .length = length}};

    memcpy(args_return_address(args), &value, sizeof(value));

    return true;
}

void args_return_struct(Args *args, const void *data, size_t size) {
    assert(data && "a C body returned a struct from nothing");

    const Type *return_type = args->symbol->func.return_type;

    assert(return_type && type_registry_size_of(args_registry(args), return_type) == size &&
           "a struct was returned at a size the declared return type does not have");
    (void)return_type;
    (void)size;

    memcpy(args_return_address(args), data, size);
}
