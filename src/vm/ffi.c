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

    /* An array decays to the address of its elements, so its argument is the slot itself rather than
     * what the slot holds; libffi is handed a cell pointing at it. */
    uint32_t decays;

    /* 0 when the declaration returns nothing, which is what tells the call to pass libffi no destination. */
    size_t return_width;

    /* A struct is written at its own size, so it lands in the return slot directly; a scalar is widened
     * to a word by libffi and needs a cell of that width to land in first. */
    bool returns_struct;
};

static ffi_type *ffi_type_of(Arena *arena, TypeRegistry *registry, const Function *function, const Type *type,
                             const char **out_reason);

/* A '&str', an array and an owning block are an address with one or two lengths in a single value,
 * so C receives the value struct the VM already holds rather than a pointer to it. */
static ffi_type *ffi_value_struct_type(Arena *arena, size_t int_count, const size_t *offsets, size_t size,
                                       const char **out_reason) {
    ffi_type *descriptor = arena_alloc(arena, sizeof(ffi_type));
    ffi_type **elements = arena_alloc(arena, (int_count + 2) * sizeof(ffi_type *));
    size_t *computed = arena_alloc(arena, (int_count + 1) * sizeof(size_t));

    if (!descriptor || !elements || !computed) {
        *out_reason = "out of memory";
        return NULL;
    }

    elements[0] = &ffi_type_pointer;

    for (size_t i = 0; i < int_count; i++) {
        elements[1 + i] = &ffi_type_sint32;
    }

    elements[int_count + 1] = NULL;

    *descriptor = (ffi_type){.type = FFI_TYPE_STRUCT, .elements = elements};

    if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, descriptor, computed) != FFI_OK || descriptor->size != size) {
        *out_reason = "an address and a length do not reach C as the value struct the VM holds";
        return NULL;
    }

    for (size_t i = 0; i < int_count + 1; i++) {
        if (computed[i] != offsets[i]) {
            *out_reason = "an address and a length do not reach C as the value struct the VM holds";
            return NULL;
        }
    }

    return descriptor;
}

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

static ffi_type *ffi_struct_type_of(Arena *arena, TypeRegistry *registry, const Function *function,
                                    const Type *type, const char **out_reason) {
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
        elements[i] = ffi_type_of(arena, registry, function, fields->fields[i].type, out_reason);

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

static ffi_type *ffi_type_of(Arena *arena, TypeRegistry *registry, const Function *function, const Type *type,
                             const char **out_reason) {
    if (!type) {
        return &ffi_type_void;
    }

    switch (type_kind(type)) {
    case TYPE_INT:
        return &ffi_type_sint32;

    case TYPE_FLOAT:
        return &ffi_type_float;

    /* A bool is one byte in a struct but a whole widened slot on its own, which is what the VM reads
     * a returned one from and what a parameter's slot holds. */
    case TYPE_BOOL:
        return &ffi_type_sint32;

    case TYPE_BYTE:
        return &ffi_type_uint8;

    case TYPE_PTR:
        return &ffi_type_pointer;

    case TYPE_STRUCT:
        return ffi_struct_type_of(arena, registry, function, type, out_reason);

    default:
        break;
    }

    /* A fixed-size array is its elements inline, which is what C passes as a pointer to the first. */
    if (type_kind(type) == TYPE_ARRAY) {
        return &ffi_type_pointer;
    }

    if (type_is_str_ref(type)) {
        static const size_t offsets[] = {offsetof(StrRef, data), offsetof(StrRef, length)};

        return ffi_value_struct_type(arena, 1, offsets, sizeof(StrRef), out_reason);
    }

    if (type_kind(type) == TYPE_BLOCK) {
        static const size_t offsets[] = {offsetof(BlockValue, data), offsetof(BlockValue, capacity),
                                         offsetof(BlockValue, length)};

        return ffi_value_struct_type(arena, 2, offsets, sizeof(BlockValue), out_reason);
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
                                    void *symbol, const char **out_reason) {
    FfiSignature *signature = arena_alloc(arena, sizeof(FfiSignature));

    if (!signature) {
        *out_reason = "out of memory";
        return NULL;
    }

    /* The context spends one leading element ahead of the declared parameters. */
    size_t max_args = function->param_count + 1;

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

    params[0] = &ffi_type_pointer;

    uint32_t decays = 0;

    for (size_t i = 0; i < function->param_count; i++) {
        params[1 + i] = ffi_type_of(arena, registry, function, function->params[i], out_reason);

        if (!params[1 + i]) {
            return NULL;
        }

        if (type_kind(function->params[i]) == TYPE_ARRAY) {
            decays |= 1u << i;
        }

        /* A parameter the specialization chose has no one C type across instantiations, so the body
         * is handed its address and reads gab_ctx_type_size bytes from it. */
        if (function->decl->params_by_address & (1u << i)) {
            params[1 + i] = &ffi_type_pointer;
            decays |= 1u << i;
        }
    }

    /* A declaration returning what its specialization chose has no one C return type across
     * instantiations, so the body writes the slot through gab_ctx_return and returns nothing. */
    bool returns_by_slot = function->decl->returns_a_type_param;

    ffi_type *result = returns_by_slot
                           ? &ffi_type_void
                           : ffi_type_of(arena, registry, function, function->return_type, out_reason);

    if (!result) {
        return NULL;
    }

    if (ffi_prep_cif(&signature->cif, FFI_DEFAULT_ABI, (unsigned int)max_args, result, params) != FFI_OK) {
        *out_reason = "the declaration does not describe a callable C signature";
        return NULL;
    }

    signature->symbol = symbol;
    signature->param_count = function->param_count;
    signature->decays = decays;
    signature->return_width = (function->return_type && !returns_by_slot)
                                  ? type_registry_size_of(registry, function->return_type)
                                  : 0;

    if (function->return_type && type_kind(function->return_type) == TYPE_BOOL) {
        signature->return_width = sizeof(int32_t);
    }
    signature->returns_struct = result->type == FFI_TYPE_STRUCT;

    return signature;
}

void ffi_invoke(const FfiSignature *signature, Args *args) {
    void *values[FFI_MAX_PARAMS];
    void *addresses[FFI_MAX_PARAMS];

    values[0] = (void *)&args;

    for (size_t i = 0; i < signature->param_count; i++) {
        addresses[i] = args_address(args, (int)i, NULL);

        values[1 + i] = (signature->decays >> i) & 1u ? (void *)&addresses[i] : addresses[i];
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
