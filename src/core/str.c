#include "core/str.h"
#include "core/core.h"
#include "library.h"

#include "gab.h"
#include "object.h"
#include "scope.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int32_t string_find(StrRef haystack, StrRef needle, int32_t from) {
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

static int32_t string_len(GabCtx *ctx, StrRef self) {
    (void)ctx;

    return self.length;
}

static int32_t string_is_empty(GabCtx *ctx, StrRef self) {
    (void)ctx;

    return self.length == 0;
}

static int32_t string_at(GabCtx *ctx, StrRef self, int32_t index) {
    if (index < 0 || (size_t)index >= (size_t)self.length) {
        gab_ctx_fail(ctx, "string index is out of range");
        return 0;
    }

    return (unsigned char)self.data[index];
}

static int32_t string_starts_with(GabCtx *ctx, StrRef self, StrRef prefix) {
    (void)ctx;

    return prefix.length <= self.length && memcmp(self.data, prefix.data, prefix.length) == 0;
}

static int32_t string_ends_with(GabCtx *ctx, StrRef self, StrRef suffix) {
    (void)ctx;

    return suffix.length <= self.length &&
           memcmp(self.data + (self.length - suffix.length), suffix.data, suffix.length) == 0;
}

static int32_t string_contains(GabCtx *ctx, StrRef self, StrRef needle) {
    (void)ctx;

    return string_find(self, needle, 0) >= 0;
}

static int32_t string_index_of(GabCtx *ctx, StrRef self, StrRef needle) {
    (void)ctx;

    return string_find(self, needle, 0);
}

static int32_t string_count(GabCtx *ctx, StrRef self, StrRef needle) {
    (void)ctx;

    if (needle.length == 0) {
        return 0;
    }

    int32_t total = 0;

    for (int32_t at = 0; (at = string_find(self, needle, at)) >= 0; at += (int32_t)needle.length) {
        total++;
    }

    return total;
}

static const char CORE_SRC[] = "module " GAB_CORE_MODULE ";\n"
                               "impl str {\n"
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
        GabExternFn symbol;
    } METHODS[] = {
        {"len", (GabExternFn)string_len},
        {"is_empty", (GabExternFn)string_is_empty},
        {"at", (GabExternFn)string_at},
        {"starts_with", (GabExternFn)string_starts_with},
        {"ends_with", (GabExternFn)string_ends_with},
        {"contains", (GabExternFn)string_contains},
        {"index_of", (GabExternFn)string_index_of},
        {"count", (GabExternFn)string_count},
    };

    Library core = library_open(vm, GAB_CORE_MODULE, true);

    for (size_t i = 0; i < sizeof(METHODS) / sizeof(*METHODS); i++) {
        library_extern(&core, "str", METHODS[i].name, METHODS[i].symbol);
    }

    library_declare_source(&core, CORE_SRC);
}
