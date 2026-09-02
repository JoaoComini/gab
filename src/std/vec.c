#include "library.h"
#include "std/std.h"

#include "allocator.h"
#include "arena.h"
#include "gab.h"
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

static size_t vec_stride(GabCtx *ctx) { return gab_ctx_type_size(ctx, 0); }

/* A slot is four bytes, so a header the stack names is not aligned for a VecHeader and is copied
 * out and back rather than written through. */
static void vec_push(GabCtx *ctx, void *self, const void *value) {
    size_t stride = vec_stride(ctx);

    VecHeader vec;
    memcpy(&vec, self, sizeof(vec));

    if (!block_reserve(&DEFAULT_ALLOCATOR, &vec.block, 1, stride)) {
        gab_ctx_fail(ctx, "out of memory growing a vector");
        return;
    }

    memcpy((char *)vec.block.data + (size_t)vec.block.length * stride, value, stride);

    vec.block.length++;

    memcpy(self, &vec, sizeof(vec));
}

static void vec_at(GabCtx *ctx, const void *self, int32_t index) {
    VecHeader vec;
    memcpy(&vec, self, sizeof(vec));

    if (index < 0 || index >= vec.block.length) {
        gab_ctx_fail(ctx, "vector index is out of range");
        return;
    }

    size_t stride = vec_stride(ctx);

    memcpy(gab_ctx_return(ctx), (const char *)vec.block.data + (size_t)index * stride, stride);
}

static int32_t vec_len(GabCtx *ctx, const void *self) {
    (void)ctx;

    VecHeader vec;
    memcpy(&vec, self, sizeof(vec));

    return vec.block.length;
}

static VecHeader vec_new(GabCtx *ctx, int32_t count) {
    VecHeader vec = {0};

    if (count < 0) {
        gab_ctx_fail(ctx, "a vector cannot reserve a negative count");
        return vec;
    }

    if (count > 0 && !block_reserve(&DEFAULT_ALLOCATOR, &vec.block, count, vec_stride(ctx))) {
        gab_ctx_fail(ctx, "out of memory reserving a vector");
    }

    return vec;
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
        void *symbol;
    } METHODS[] = {
        {"new", (void *)(uintptr_t)vec_new},
        {"push", (void *)(uintptr_t)vec_push},
        {"at", (void *)(uintptr_t)vec_at},
        {"len", (void *)(uintptr_t)vec_len},
    };

    for (size_t i = 0; i < sizeof(METHODS) / sizeof(*METHODS); i++) {
        library_extern(&std, "Vec", METHODS[i].name, METHODS[i].symbol);
    }

    library_declare_source(&std, VEC_SRC);
}
