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

// The 'Vec' declaration: a block of the element it is given.
//
// A declaration, so no width is settled here -- what a vector is laid out as
// follows from the element, and that is not known until one is applied.
static const TypeDef *vec_declare_type(VM *vm) {
    Arena *arena = vm->env.arena;
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    // The block owns the memory and carries both the capacity it was taken at
    // and how far into it anything was written, so freeing it asks nothing
    // else -- which is the whole of what a vector is.
    TypeField *fields = arena_alloc(arena, sizeof(TypeField));

    fields[0] = (TypeField){
        .name = string_from_cstr(&vm->env.strings, "data"),
        .type = type_registry_block_of(registry, type_registry_param(registry, 0)),
    };

    // Arena-allocated rather than local: every instantiation reads this, and
    // the declaration outlives the call that made it.
    TypeDef *def = arena_alloc(arena, sizeof(TypeDef));

    *def = (TypeDef){
        .name = string_from_cstr(&vm->env.strings, "Vec"),
        .param_count = 1,
        .fields = fields,
        .field_count = 1,
    };

    const TypeDecl decl = {.def = def};

    builtin_declare_type(vm, &decl);

    return def;
}

// A vector's header: the block it owns, which carries how far into it anything
// was written.
//
// Copied in and out rather than pointed at. A frame slot is aligned to the slot
// width, which is narrower than this struct asks for, so reading one in place
// is undefined however well it happens to work.
typedef struct {
    GabBlockValue block;
} VecHeader;

// The receiver's header, read out of the slots the pointer names.
static VecHeader vec_load(Args *args) {
    VecHeader vec;
    memcpy(&vec, args_pointer(args, 0), sizeof(vec));

    return vec;
}

// Writes a header back where it was read from. Only the methods that change one
// call this: 'at' and 'len' leave the vector as they found it.
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

// 'v.push(x)'. Writes one element past the live ones, growing first if there is
// no room.
//
// The element is copied in as bytes, which is what makes pushing something that
// owns a move: the caller's slot and this one would otherwise both free it, and
// the resolver is what refuses the copy that would.
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

// 'v.at(i)'. The element at an index.
//
// An index outside the live elements fails the run rather than answering: the
// slots past them hold whatever the block was zeroed to, and there is no value
// that could mean 'no element' without a caller mistaking it for one.
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

// 'v.len()'. How many elements have been pushed, which the block carries.
static void vec_len(Args *args) { args_return_int(args, vec_load(args).block.length); }

void builtin_register_vec(VM *vm) {
    // Declared here and held as a local: what a provider gets back is its
    // handle to the declaration, and one VM's types are not another's.
    const TypeDef *vec_def = vec_declare_type(vm);

    TypeRegistry *registry = vm->env.global_scope.type_registry;

    Arena *arena = vm->env.arena;

    // Every method reaches the header in place, so each takes a pointer to it:
    // a receiver by value would copy a vector, which owns its block and so
    // cannot be copied at all.
    //
    // Written as a reference to the declaration applied to its own parameter,
    // which is what 'Vec<T>' is from inside the declaration. Substituting it
    // finds the instantiation being built, so no receiver needs naming a self
    // the declaration could not otherwise say.
    const Type *element = type_registry_param(registry, 0);
    const Type *self = type_registry_instantiate(registry, vec_def, &element, 1);
    const Type *receiver = type_registry_ref_to(registry, self);

    const Type *an_int = type_registry_get_primitive(registry, TYPE_INT);

    // Arena-allocated rather than local: the declaration holds these for as
    // long as the VM lives, since an instantiation reads them whenever one is
    // first named.
    const Type **push_params = arena_alloc(arena, sizeof(const Type *));
    const Type **at_params = arena_alloc(arena, sizeof(const Type *));
    GenericMethod *methods = arena_alloc(arena, 3 * sizeof(GenericMethod));

    push_params[0] = element;
    at_params[0] = an_int;

    methods[0] = (GenericMethod){
        .name = string_from_cstr(&vm->env.strings, "push"),
        .body = (void *)vec_push,
        .receiver = receiver,

        // Nothing: a push is done for what it leaves in the vector, and a type
        // returning nothing is what a NULL return type is everywhere.
        .result = NULL,
        .params = push_params,
        .param_count = 1,
    };

    methods[1] = (GenericMethod){
        .name = string_from_cstr(&vm->env.strings, "at"),
        .body = (void *)vec_at,
        .receiver = receiver,
        .result = element,
        .params = at_params,
        .param_count = 1,
    };

    methods[2] = (GenericMethod){
        .name = string_from_cstr(&vm->env.strings, "len"),
        .body = (void *)vec_len,
        .receiver = receiver,
        .result = an_int,
        .params = NULL,
        .param_count = 0,
    };

    // Written into the declaration rather than registered against a type: what
    // answers these is every 'Vec<T>', and none of them exists yet.
    TypeDef *def = (TypeDef *)vec_def;

    def->methods = methods;
    def->method_count = 3;
}
