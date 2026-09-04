#include "std.h"

#include "gab.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    GabBlock block;
} Vec;

static Vec vec_load(GabCtx *ctx) {
    Vec vec;
    memcpy(&vec, gab_ctx_self(ctx), sizeof(vec));

    return vec;
}

static void vec_store(GabCtx *ctx, const Vec *vec) { memcpy(gab_ctx_self(ctx), vec, sizeof(*vec)); }

static void vec_push(GabCtx *ctx) {
    Vec vec = vec_load(ctx);
    size_t stride = gab_ctx_type_size(ctx, 0);

    if (!gab_block_reserve(ctx, &vec.block, 1, stride)) {
        gab_ctx_fail(ctx, GAB_FAIL_OUT_OF_MEMORY, "out of memory growing a vector");
        return;
    }

    memcpy((char *)vec.block.data + (size_t)vec.block.length * stride, gab_ctx_address(ctx, 1), stride);

    vec.block.length++;

    vec_store(ctx, &vec);
}

static void vec_index(GabCtx *ctx) {
    Vec vec = vec_load(ctx);
    int32_t index = gab_ctx_int(ctx, 1);

    if (index < 0 || index >= vec.block.length) {
        gab_ctx_fail(ctx, GAB_FAIL_RUNTIME, "vector index is out of range");
        return;
    }

    size_t stride = gab_ctx_type_size(ctx, 0);

    gab_ctx_return_pointer(ctx, (char *)vec.block.data + (size_t)index * stride);
}

static void vec_len(GabCtx *ctx) { gab_ctx_return_int(ctx, vec_load(ctx).block.length); }

static void vec_new(GabCtx *ctx) {
    int32_t count = gab_ctx_int(ctx, 0);

    if (count < 0) {
        gab_ctx_fail(ctx, GAB_FAIL_RUNTIME, "a vector cannot reserve a negative count");
        return;
    }

    size_t stride = gab_ctx_type_size(ctx, 0);

    Vec vec = {0};

    if (count > 0 && !gab_block_reserve(ctx, &vec.block, count, stride)) {
        gab_ctx_fail(ctx, GAB_FAIL_OUT_OF_MEMORY, "out of memory reserving a vector");
        return;
    }

    gab_ctx_return_struct(ctx, &vec, sizeof vec);
}

static const char VEC_SRC[] = "impl<T> Vec<T> {\n"
                              "    extern func new(count: int): Self;\n"
                              "    extern func push(self: &Self, value: T);\n"
                              "    extern func index(self: &Self, at: int): &T;\n"
                              "    extern func len(self: &Self): int;\n"
                              "}\n"
                              "impl<T> Vec<T> as Index<T> {}\n";

void std_register_vec(GabVM *vm) {
    GabError err;

    GabLib *std = gab_lib_open(vm, "std", &err);
    assert(std && "the standard library opens");

    const GabFieldSpec fields[] = {
        {"data", gab_lib_block_of(std, gab_lib_param(std, 0))},
    };

    const GabTypeSpec spec = {
        .name = "Vec",
        .params = 1,
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(*fields),
    };

    bool ok = gab_lib_type(std, &spec, &err) == NULL;
    assert(ok && "a generic type declares without instantiating");
    (void)ok;

    static const struct {
        const char *name;
        GabExternFn body;
    } METHODS[] = {
        {"new", vec_new},
        {"push", vec_push},
        {"index", vec_index},
        {"len", vec_len},
    };

    for (size_t i = 0; i < sizeof(METHODS) / sizeof(*METHODS); i++) {
        bool bound = gab_lib_bind(std, "Vec", METHODS[i].name, METHODS[i].body, &err);

        assert(bound && "a library binds each of its externs once");
        (void)bound;
    }

    bool loaded = gab_lib_source(std, VEC_SRC, &err);

    assert(loaded && "a library's declarations compile");
    (void)loaded;

    gab_lib_close(std);
}
