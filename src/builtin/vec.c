#include "builtin/builtin.h"

#include "allocator.h"
#include "arena.h"
#include "object.h"
#include "type_registry.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <stdint.h>
#include <string.h>

// What an empty vector's first allocation holds. Small enough that a vector
// pushed into once does not reserve a page, large enough that the first few
// pushes do not each reallocate.
#define VEC_INITIAL_CAPACITY 4

// A vector's header, as its two fields are laid out: the block it owns and how
// far into that block has been written.
//
// Copied in and out rather than pointed at. A frame slot is aligned to the
// slot width, which is narrower than this struct asks for, so reading one in
// place is undefined however well it happens to work.
typedef struct {
    GabBlockValue block;
    int32_t length;
} VecHeader;

// The receiver's header, read out of the slots the pointer names.
static VecHeader vec_load(Args *args) {
    VecHeader vec;
    memcpy(&vec, args_pointer(args, 0), sizeof(vec));

    return vec;
}

// Writes a header back where it was read from. Only the two methods that change
// one call this: 'at' and 'len' leave the vector as they found it.
static void vec_store(Args *args, const VecHeader *vec) { memcpy(args_pointer(args, 0), vec, sizeof(*vec)); }

// How wide one element of the receiver's vector is. Read off the declared
// parameter rather than passed in: the signature was substituted for this
// instantiation, so what 'push' takes is exactly what the block strides by.
static size_t vec_stride(Args *args) {
    const Type *receiver = NULL;
    args_address(args, 0, &receiver);

    return type_registry_size_of(args->vm->env.global_scope.type_registry,
                                 type_pointee(type_fields(type_pointee(receiver))[0].type));
}

// Makes room for one more element, growing the block if every slot it has is
// live. Doubling, so that pushing n elements copies O(n) times rather than
// once per push.
//
// False when the allocation fails, having failed the run: the caller must not
// then write the element it was making room for.
static bool vec_reserve_one(Args *args, VecHeader *vec, size_t stride) {
    if (vec->length < vec->block.capacity) {
        return true;
    }

    // Doubling overflows a signed int past 2^30 elements, which is undefined
    // rather than merely wrong. A vector that large has no room to grow, so the
    // run fails here rather than wrapping to a smaller block than it holds.
    if (vec->block.capacity > INT32_MAX / 2) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "a vector cannot grow any further");
        return false;
    }

    int32_t capacity = vec->block.capacity ? vec->block.capacity * 2 : VEC_INITIAL_CAPACITY;

    void *block = DEFAULT_ALLOCATOR.alloc(DEFAULT_ALLOCATOR.ctx, (size_t)capacity * stride);

    if (!block) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory growing a vector");
        return false;
    }

    // The live elements move as bytes: what one of them owns is owned by
    // whichever slot holds it, and that is the new block once this returns.
    if (vec->length) {
        memcpy(block, vec->block.data, (size_t)vec->length * stride);
    }

    // Zeroed past the live ones for the reason every allocation is: an element
    // that owns and was never written must read as NULL, since the drop walk
    // has no other way to tell it from a live reference.
    memset((char *)block + (size_t)vec->length * stride, 0,
           ((size_t)capacity - (size_t)vec->length) * stride);

    if (vec->block.data) {
        DEFAULT_ALLOCATOR.free_sized(DEFAULT_ALLOCATOR.ctx, vec->block.data,
                                     (size_t)vec->block.capacity * stride);
    }

    vec->block.data = block;
    vec->block.capacity = capacity;

    return true;
}

// 'v.push(x)'. Writes one element past the live ones, growing first if there is
// no room.
//
// The element is copied in as bytes, which is what makes pushing something that
// owns a move: the caller's slot and this one would otherwise both free it, and
// the resolver is what refuses the copy that would.
static void vec_push(Args *args) {
    VecHeader vec = vec_load(args);
    size_t stride = vec_stride(args);

    if (!vec_reserve_one(args, &vec, stride)) {
        return;
    }

    const Type *element = NULL;
    const uint8_t *value = args_address(args, 1, &element);

    memcpy((char *)vec.block.data + (size_t)vec.length * stride, value, stride);

    vec.length++;

    vec_store(args, &vec);
}

// 'v.at(i)'. The element at an index.
//
// An index outside the live elements fails the run rather than answering: the
// slots past them hold whatever the block was zeroed to, and there is no value
// that could mean 'no element' without a caller mistaking it for one.
static void vec_at(Args *args) {
    VecHeader vec = vec_load(args);
    int32_t index = args_int(args, 1);

    if (index < 0 || index >= vec.length) {
        vm_fail(args->vm, VM_RUN_ERR_EXTERN, "vector index is out of range");
        return;
    }

    size_t stride = vec_stride(args);

    args_return_struct(args, (const char *)vec.block.data + (size_t)index * stride, stride);
}

// 'v.len()'. How many elements have been pushed, which the header carries.
static void vec_len(Args *args) { args_return_int(args, vec_load(args).length); }

// Declares one substituted method on one instantiation. The registry calls this
// where a 'Vec<T>' is interned, having filled the parameter in.
//
// The signature arrives receiver-first, which is how a method's parameters are
// numbered everywhere: what the caller writes is everything after it.
static void vec_install_method(void *ctx, const Type *on, const char *name, void *body,
                               const Type *return_type, const Type *const *signature, size_t count) {
    builtin_register_method(ctx, on, signature[0], name, (GabExternFn)body, return_type, signature + 1,
                            count - 1);
}

void builtin_register_vec(VM *vm) {
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    Arena *arena = vm->env.arena;

    // Every method reaches the header in place, so each takes a pointer to it:
    // a receiver by value would copy a vector, which owns its block and so
    // cannot be copied at all.
    const GenericField receiver = {.from = GENERIC_FROM_SELF, .ctor = TYPE_CTOR_REF};

    // The element, as the parameter it was declared over. What 'push' takes and
    // what 'at' hands back are the same type, filled in per instantiation.
    const GenericField element = {.from = GENERIC_FROM_PARAM, .param = 0, .ctor = TYPE_CTOR_NOMINAL};

    const GenericField an_int = {.fixed = registry->builtins.int_type};

    // Arena-allocated rather than local: the declaration holds these for as
    // long as the VM lives, since an instantiation reads them whenever one is
    // first named.
    GenericField *push_params = arena_alloc(arena, sizeof(GenericField));
    GenericField *at_params = arena_alloc(arena, sizeof(GenericField));
    GenericMethod *methods = arena_alloc(arena, 3 * sizeof(GenericMethod));

    push_params[0] = element;
    at_params[0] = an_int;

    methods[0] = (GenericMethod){
        .name = "push",
        .body = (void *)vec_push,
        .receiver = receiver,

        // Nothing: a push is done for what it leaves in the vector, and a type
        // returning nothing is what a NULL return type is everywhere.
        .result = (GenericField){.from = GENERIC_FROM_NOTHING},
        .params = push_params,
        .param_count = 1,
    };

    methods[1] = (GenericMethod){
        .name = "at",
        .body = (void *)vec_at,
        .receiver = receiver,
        .result = element,
        .params = at_params,
        .param_count = 1,
    };

    methods[2] = (GenericMethod){
        .name = "len",
        .body = (void *)vec_len,
        .receiver = receiver,
        .result = an_int,
        .params = NULL,
        .param_count = 0,
    };

    // Written into the declaration rather than registered against a type: what
    // answers these is every 'Vec<T>', and none of them exists yet.
    GenericDecl *generic = (GenericDecl *)registry->builtins.vec_type->generic;

    generic->methods = methods;
    generic->method_count = 3;

    // How an instantiation's methods become Symbols. Registered once, so that a
    // 'Vec<T>' first named by a later compile is declared the same way.
    registry->install_method = vec_install_method;
    registry->install_ctx = vm;
}
