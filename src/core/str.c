#include "core/core.h"
#include "library.h"

#include "gab.h"
#include "object.h"
#include "scope.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int32_t string_find(GabStrRef haystack, GabStrRef needle, int32_t from) {
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

static void string_len(Args *args) { args_return_int(args, args_string(args, 0).length); }

static void string_is_empty(Args *args) { args_return_bool(args, args_string(args, 0).length == 0); }

static void string_at(Args *args) {
    GabStrRef string = args_string(args, 0);
    int32_t index = args_int(args, 1);

    if (index < 0 || (size_t)index >= (size_t)string.length) {
        vm_fail(args->vm, VM_RUN_ERR_EXTERN, "string index is out of range");
        return;
    }

    args_return_int(args, (unsigned char)string.data[index]);
}

static void string_starts_with(Args *args) {
    GabStrRef string = args_string(args, 0);
    GabStrRef prefix = args_string(args, 1);

    args_return_bool(args,
                     prefix.length <= string.length && memcmp(string.data, prefix.data, prefix.length) == 0);
}

static void string_ends_with(Args *args) {
    GabStrRef string = args_string(args, 0);
    GabStrRef suffix = args_string(args, 1);

    args_return_bool(args,
                     suffix.length <= string.length && memcmp(string.data + (string.length - suffix.length),
                                                              suffix.data, suffix.length) == 0);
}

static void string_contains(Args *args) {
    args_return_bool(args, string_find(args_string(args, 0), args_string(args, 1), 0) >= 0);
}

static void string_index_of(Args *args) {
    args_return_int(args, string_find(args_string(args, 0), args_string(args, 1), 0));
}

static void string_count(Args *args) {
    GabStrRef string = args_string(args, 0);
    GabStrRef needle = args_string(args, 1);

    if (needle.length == 0) {
        args_return_int(args, 0);
        return;
    }

    int32_t total = 0;

    for (int32_t at = 0; (at = string_find(string, needle, at)) >= 0; at += (int32_t)needle.length) {
        total++;
    }

    args_return_int(args, total);
}

static const char CORE_SRC[] = "module " GAB_PRELUDE_MODULE ";\n"
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

    GabLibrary prelude = library_open(vm, GAB_PRELUDE_MODULE, true);

    for (size_t i = 0; i < sizeof(METHODS) / sizeof(*METHODS); i++) {
        library_extern(&prelude, "str", METHODS[i].name, METHODS[i].body);
    }

    library_declare_source(&prelude, CORE_SRC);
}
