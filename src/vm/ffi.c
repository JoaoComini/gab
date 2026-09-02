#include "vm/ffi.h"

#include "object.h"
#include "type/type.h"
#include "type/type_layout.h"
#include "vm/args.h"

#include <ffi.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The call site builds its argument vector on the stack, so a declaration wider than this is refused
 * at prepare time rather than overrunning it. */
#define FFI_MAX_PARAMS 32

struct FfiSignature {
    void *symbol;

    ffi_cif cif;

    size_t param_count;

    /* Whether a GabCtx * leads the declared parameters. */
    bool wants_ctx;

    /* An array decays to the address of its elements, so its argument is the slot itself rather than
     * what the slot holds; libffi is handed a cell pointing at it. */
    uint32_t decays;

    /* 0 when the declaration returns nothing, which is what tells the call to pass libffi no destination. */
    size_t return_width;

    /* A struct is written at its own size, so it lands in the return slot directly; a scalar is widened
     * to a word by libffi and needs a cell of that width to land in first. */
    bool returns_struct;
};

static ffi_type *ffi_type_of(Arena *arena, TypeRegistry *registry, const Type *type, const char **out_reason);

/* A '&str' and an array are an address and a length in one value, so C receives the value struct the
 * VM already holds -- StrRef and ArrayValue -- rather than a pointer to it. */
static ffi_type *ffi_value_struct_type(Arena *arena, size_t length_offset, size_t size,
                                       const char **out_reason) {
    ffi_type *descriptor = arena_alloc(arena, sizeof(ffi_type));
    ffi_type **elements = arena_alloc(arena, 3 * sizeof(ffi_type *));

    if (!descriptor || !elements) {
        *out_reason = "out of memory";
        return NULL;
    }

    elements[0] = &ffi_type_pointer;
    elements[1] = &ffi_type_sint32;
    elements[2] = NULL;

    *descriptor = (ffi_type){.type = FFI_TYPE_STRUCT, .elements = elements};

    size_t offsets[2];

    if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, descriptor, offsets) != FFI_OK || descriptor->size != size ||
        offsets[0] != 0 || offsets[1] != length_offset) {
        *out_reason = "an address and a length do not reach C as the value struct the VM holds";
        return NULL;
    }

    return descriptor;
}

/* libffi computes a struct's layout from its element list, and the language's premise is that the
 * result is the C layout the registry already recorded. Disagreement means one of the two is wrong
 * about the ABI, so the load fails rather than the call passing bytes the callee reads differently. */
static bool ffi_struct_layout_agrees(Arena *arena, TypeRegistry *registry, const Type *type,
                                     ffi_type *descriptor, const char **out_reason) {
    const TypeFields *fields = type_registry_fields_of(registry, type);
    const TypeLayout *layout = type_registry_layout_of(registry, type);

    size_t *offsets = arena_alloc(arena, fields->count * sizeof(size_t));

    if (!offsets) {
        *out_reason = "out of memory";
        return false;
    }

    if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, descriptor, offsets) != FFI_OK) {
        *out_reason = "a struct in this declaration has no C layout";
        return false;
    }

    if (descriptor->size != layout->size || descriptor->alignment != layout->alignment) {
        *out_reason = "a struct in this declaration does not have the size and alignment C gives it";
        return false;
    }

    for (size_t i = 0; i < fields->count; i++) {
        if (offsets[i] != layout->offsets[i]) {
            *out_reason = "a field of a struct in this declaration does not sit where C puts it";
            return false;
        }
    }

    return true;
}

static ffi_type *ffi_struct_type_of(Arena *arena, TypeRegistry *registry, const Type *type,
                                    const char **out_reason) {
    const TypeFields *fields = type_registry_fields_of(registry, type);

    if (!fields || fields->count == 0) {
        *out_reason = "a struct with no fields cannot be passed to C";
        return NULL;
    }

    ffi_type *descriptor = arena_alloc(arena, sizeof(ffi_type));

    /* libffi reads the element list until a NULL terminator rather than from a count. */
    ffi_type **elements = arena_alloc(arena, (fields->count + 1) * sizeof(ffi_type *));

    if (!descriptor || !elements) {
        *out_reason = "out of memory";
        return NULL;
    }

    for (size_t i = 0; i < fields->count; i++) {
        elements[i] = ffi_type_of(arena, registry, fields->fields[i].type, out_reason);

        if (!elements[i]) {
            return NULL;
        }
    }

    elements[fields->count] = NULL;

    *descriptor = (ffi_type){.type = FFI_TYPE_STRUCT, .elements = elements};

    if (!ffi_struct_layout_agrees(arena, registry, type, descriptor, out_reason)) {
        return NULL;
    }

    return descriptor;
}

static ffi_type *ffi_type_of(Arena *arena, TypeRegistry *registry, const Type *type,
                             const char **out_reason) {
    if (!type) {
        return &ffi_type_void;
    }

    switch (type_kind(type)) {
    case TYPE_INT:
        return &ffi_type_sint32;

    case TYPE_FLOAT:
        return &ffi_type_float;

    /* A bool occupies a whole int32 slot, so C sees the widened value rather than a one-byte _Bool. */
    case TYPE_BOOL:
        return &ffi_type_sint32;

    case TYPE_BYTE:
        return &ffi_type_uint8;

    case TYPE_PTR:
        return &ffi_type_pointer;

    case TYPE_STRUCT:
        return ffi_struct_type_of(arena, registry, type, out_reason);

    default:
        break;
    }

    /* A fixed-size array is its elements inline, which is what C passes as a pointer to the first. */
    if (type_kind(type) == TYPE_ARRAY) {
        return &ffi_type_pointer;
    }

    if (type_is_str_ref(type)) {
        return ffi_value_struct_type(arena, offsetof(StrRef, length), sizeof(StrRef), out_reason);
    }

    /* A borrow of a type carrying metadata is a value struct, not an address; only the bare-address
     * case is a pointer. */
    if (type_is_indirect(type) && type_registry_size_of(registry, type) == sizeof(void *)) {
        return &ffi_type_pointer;
    }

    *out_reason = "a C symbol can be declared with int, float, bool, byte, pointer, str, array, and "
                  "struct types only";

    return NULL;
}

FfiSignature *ffi_signature_prepare(Arena *arena, TypeRegistry *registry, const Function *function,
                                    void *symbol, bool wants_ctx, const char **out_reason) {
    FfiSignature *signature = arena_alloc(arena, sizeof(FfiSignature));

    if (!signature) {
        *out_reason = "out of memory";
        return NULL;
    }

    /* A context spends one leading element ahead of the declared parameters. */
    size_t max_args = function->param_count + (wants_ctx ? 1 : 0);

    if (max_args > FFI_MAX_PARAMS) {
        *out_reason = "a C symbol cannot be declared with more than 32 parameters";
        return NULL;
    }

    ffi_type **params = NULL;

    if (max_args) {
        params = arena_alloc(arena, max_args * sizeof(ffi_type *));

        if (!params) {
            *out_reason = "out of memory";
            return NULL;
        }
    }

    size_t lead = wants_ctx ? 1 : 0;

    if (wants_ctx) {
        params[0] = &ffi_type_pointer;
    }

    uint32_t decays = 0;

    for (size_t i = 0; i < function->param_count; i++) {
        params[lead + i] = ffi_type_of(arena, registry, function->params[i], out_reason);

        if (!params[lead + i]) {
            return NULL;
        }

        if (type_kind(function->params[i]) == TYPE_ARRAY) {
            decays |= 1u << i;
        }
    }

    /* An owning return is only sound when the body allocates it through the VM, which needs a context
     * to reach gab_box; without one the VM would drop memory C allocated with its own allocator. */
    if (!wants_ctx && function->return_type && type_registry_owns(registry, function->return_type)) {
        *out_reason = "a C symbol returning an owning value must be bound with gab_extern_c_ctx and "
                      "allocate it with gab_box";
        return NULL;
    }

    ffi_type *result = ffi_type_of(arena, registry, function->return_type, out_reason);

    if (!result) {
        return NULL;
    }

    if (ffi_prep_cif(&signature->cif, FFI_DEFAULT_ABI, (unsigned int)max_args, result, params) != FFI_OK) {
        *out_reason = "the declaration does not describe a callable C signature";
        return NULL;
    }

    signature->symbol = symbol;
    signature->param_count = function->param_count;
    signature->wants_ctx = wants_ctx;
    signature->decays = decays;
    signature->return_width =
        function->return_type ? type_registry_size_of(registry, function->return_type) : 0;
    signature->returns_struct = result->type == FFI_TYPE_STRUCT;

    return signature;
}

void ffi_invoke(const FfiSignature *signature, Args *args) {
    void *values[FFI_MAX_PARAMS];
    void *addresses[FFI_MAX_PARAMS];

    size_t lead = signature->wants_ctx ? 1 : 0;

    if (signature->wants_ctx) {
        values[0] = (void *)&args;
    }

    for (size_t i = 0; i < signature->param_count; i++) {
        addresses[i] = args_address(args, (int)i, NULL);

        values[lead + i] = (signature->decays >> i) & 1u ? (void *)&addresses[i] : addresses[i];
    }

    if (!signature->return_width) {
        ffi_call((ffi_cif *)&signature->cif, FFI_FN(signature->symbol), NULL, values);
    } else if (signature->returns_struct) {
        ffi_call((ffi_cif *)&signature->cif, FFI_FN(signature->symbol), args_return_address(args), values);
    } else {
        /* libffi widens a scalar result narrower than a word, so it needs a cell of that width to write
         * into even when the declared return type is smaller. */
        ffi_arg result;

        ffi_call((ffi_cif *)&signature->cif, FFI_FN(signature->symbol), &result, values);

        memcpy(args_return_address(args), &result, signature->return_width);
    }
}
