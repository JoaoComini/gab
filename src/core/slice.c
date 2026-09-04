#include "core/slice.h"

#include "gab.h"
#include "library.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    void *data;
    int32_t length;
} Slice;

/* The receiver is the pair itself, so it is read where it sits rather than followed as a pointer. */
static Slice slice_load(GabCtx *ctx) {
    const uint8_t *at = gab_ctx_address(ctx, 0);

    Slice slice;
    memcpy(&slice.data, at, sizeof(slice.data));
    memcpy(&slice.length, at + sizeof(slice.data), sizeof(slice.length));

    return slice;
}

static void slice_len(GabCtx *ctx) { gab_ctx_return_int(ctx, slice_load(ctx).length); }

static void slice_index(GabCtx *ctx) {
    Slice slice = slice_load(ctx);
    int32_t index = gab_ctx_int(ctx, 1);

    if (index < 0 || index >= slice.length) {
        gab_ctx_fail(ctx, GAB_FAIL_BOUNDS, "index is out of range");
        return;
    }

    gab_ctx_return_pointer(ctx, (char *)slice.data + (size_t)index * gab_ctx_type_size(ctx, 0));
}

static const char SLICE_SRC[] = "impl<T> slice<T> {\n"
                                "    extern func len(self: &slice<T>): int;\n"
                                "}\n"
                                "impl<T> slice<T> as Index<T> {\n"
                                "    extern func index(self: &slice<T>, at: int): &T;\n"
                                "}\n"
                                "impl<T, N: int> array<T, N> as Index<T> {\n"
                                "    intrinsic func index(self: &Self, at: int): &T;\n"
                                "}\n";

void core_register_slice(VM *vm) {
    static const struct {
        const char *name;
        GabExternFn body;
    } METHODS[] = {
        {"len", slice_len},
        {"index", slice_index},
    };

    GabError err;
    GabLib *core = library_open_prelude(vm, GAB_CORE_MODULE);

    for (size_t i = 0; i < sizeof(METHODS) / sizeof(*METHODS); i++) {
        bool bound = gab_lib_bind(core, "slice", METHODS[i].name, METHODS[i].body, &err);

        assert(bound && "a library binds each of its externs once");
        (void)bound;
    }

    bool loaded = gab_lib_source(core, SLICE_SRC, &err);

    assert(loaded && "a library's declarations compile");
    (void)loaded;

    gab_lib_close(core);
}
