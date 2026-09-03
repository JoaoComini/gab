#include "library.h"
#include "std/std.h"

#include "gab.h"

#include "allocator.h"
#include "arena.h"
#include "object.h"
#include "type/type_registry.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <stdint.h>
#include <string.h>

static const TypeDecl *vec_declare_type(Library *lib) {
    TypeRegistry *registry = lib->vm->env.global_scope.type_registry;

    const TypeFieldSpec fields[] = {
        {
            .name = string_from_cstr(&lib->vm->env.strings, "data"),
            .type = type_registry_block_of(registry, type_registry_param(registry, 0)),
        },
    };

    const LibraryTypeSpec spec = {
        .name = "Vec",
        .param_count = 1,
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(*fields),
    };

    return library_type(lib, &spec);
}

typedef struct {
    BlockValue block;
} VecHeader;

static VecHeader vec_load(Args *args) {
    VecHeader vec;
    memcpy(&vec, args_pointer(args, 0), sizeof(vec));

    return vec;
}

static void vec_store(Args *args, const VecHeader *vec) { memcpy(args_pointer(args, 0), vec, sizeof(*vec)); }

static size_t vec_stride(Args *args) { return gab_ctx_type_size(args, 0); }

static void vec_push(Args *args) {
    VecHeader vec = vec_load(args);
    size_t stride = vec_stride(args);

    if (!block_reserve(&DEFAULT_ALLOCATOR, &vec.block, 1, stride)) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory growing a vector");
        return;
    }

    const uint8_t *value = args_address(args, 1);

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

    if (count > 0 && !block_reserve(&DEFAULT_ALLOCATOR, &vec.block, count, stride)) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory reserving a vector");
        return;
    }

    args_return_struct(args, &vec, sizeof vec);
}

static const char VEC_SRC[] = "module " GAB_STD_MODULE ";\n"
                              "impl<T> Vec<T> {\n"
                              "    extern func new(count: int): Vec<T>;\n"
                              "    extern func push(self: &Vec<T>, value: T);\n"
                              "    extern func at(self: &Vec<T>, index: int): T;\n"
                              "    extern func len(self: &Vec<T>): int;\n"
                              "}\n";

void std_register_vec(VM *vm) {
    Library std = library_open(vm, GAB_STD_MODULE, false);

    vec_declare_type(&std);

    static const struct {
        const char *name;
        GabExternFn body;
    } METHODS[] = {
        {"new", vec_new},
        {"push", vec_push},
        {"at", vec_at},
        {"len", vec_len},
    };

    for (size_t i = 0; i < sizeof(METHODS) / sizeof(*METHODS); i++) {
        library_extern(&std, "Vec", METHODS[i].name, METHODS[i].body);
    }

    library_declare_source(&std, VEC_SRC);
}
