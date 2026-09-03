#include "core/str.h"

#include "gab.h"
#include "library.h"
#include "scope.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *data;
    int32_t length;
} Str;

static Str str_arg(GabCtx *ctx, int index) {
    Str str = {0};
    str.data = gab_ctx_string(ctx, index, &str.length);

    return str;
}

static int32_t string_find(Str haystack, Str needle, int32_t from) {
    if (needle.length > haystack.length) {
        return -1;
    }

    for (int32_t start = from; start <= (int32_t)(haystack.length - needle.length); start++) {
        if (memcmp(haystack.data + start, needle.data, needle.length) == 0) {
            return start;
        }
    }

    return -1;
}

static void string_len(GabCtx *ctx) { gab_ctx_return_int(ctx, str_arg(ctx, 0).length); }

static void string_is_empty(GabCtx *ctx) { gab_ctx_return_bool(ctx, str_arg(ctx, 0).length == 0); }

static void string_at(GabCtx *ctx) {
    Str string = str_arg(ctx, 0);
    int32_t index = gab_ctx_int(ctx, 1);

    if (index < 0 || (size_t)index >= (size_t)string.length) {
        gab_ctx_fail(ctx, GAB_FAIL_RUNTIME, "string index is out of range");
        return;
    }

    gab_ctx_return_int(ctx, (unsigned char)string.data[index]);
}

static void string_starts_with(GabCtx *ctx) {
    Str string = str_arg(ctx, 0);
    Str prefix = str_arg(ctx, 1);

    gab_ctx_return_bool(ctx, prefix.length <= string.length &&
                                 memcmp(string.data, prefix.data, prefix.length) == 0);
}

static void string_ends_with(GabCtx *ctx) {
    Str string = str_arg(ctx, 0);
    Str suffix = str_arg(ctx, 1);

    gab_ctx_return_bool(
        ctx, suffix.length <= string.length &&
                 memcmp(string.data + (string.length - suffix.length), suffix.data, suffix.length) == 0);
}

static void string_contains(GabCtx *ctx) {
    gab_ctx_return_bool(ctx, string_find(str_arg(ctx, 0), str_arg(ctx, 1), 0) >= 0);
}

static void string_index_of(GabCtx *ctx) {
    gab_ctx_return_int(ctx, string_find(str_arg(ctx, 0), str_arg(ctx, 1), 0));
}

static void string_count(GabCtx *ctx) {
    Str string = str_arg(ctx, 0);
    Str needle = str_arg(ctx, 1);

    if (needle.length == 0) {
        gab_ctx_return_int(ctx, 0);
        return;
    }

    int32_t total = 0;

    for (int32_t at = 0; (at = string_find(string, needle, at)) >= 0; at += (int32_t)needle.length) {
        total++;
    }

    gab_ctx_return_int(ctx, total);
}

static const char CORE_SRC[] = "impl str {\n"
                               "    extern func len(self: &str): int;\n"
                               "    extern func is_empty(self: &str): bool;\n"
                               "    extern func at(self: &str, index: int): int;\n"
                               "    extern func starts_with(self: &str, prefix: &str): bool;\n"
                               "    extern func ends_with(self: &str, suffix: &str): bool;\n"
                               "    extern func contains(self: &str, needle: &str): bool;\n"
                               "    extern func index_of(self: &str, needle: &str): int;\n"
                               "    extern func count(self: &str, needle: &str): int;\n"
                               "}\n";

void core_register_str(VM *vm) {
    static const struct {
        const char *name;
        GabExternFn body;
    } METHODS[] = {
        {"len", string_len},
        {"is_empty", string_is_empty},
        {"at", string_at},
        {"starts_with", string_starts_with},
        {"ends_with", string_ends_with},
        {"contains", string_contains},
        {"index_of", string_index_of},
        {"count", string_count},
    };

    GabError err;
    GabLib *core = library_open_prelude(vm, GAB_CORE_MODULE);

    for (size_t i = 0; i < sizeof(METHODS) / sizeof(*METHODS); i++) {
        bool bound = gab_lib_bind(core, "str", METHODS[i].name, METHODS[i].body, &err);

        assert(bound && "a library binds each of its externs once");
        (void)bound;
    }

    bool loaded = gab_lib_source(core, CORE_SRC, &err);

    assert(loaded && "a library's declarations compile");
    (void)loaded;

    gab_lib_close(core);
}
