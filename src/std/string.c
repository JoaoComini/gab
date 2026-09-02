#include "library.h"
#include "std/std.h"

#include "compile.h"
#include "object.h"
#include "type/type_registry.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include "allocator.h"
#include "gab.h"
#include "scope.h"

#include <assert.h>
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

static void string_push(Args *args) {
    GabStringValue string = args_string_at(args, 0);
    int32_t character = args_int(args, 1);

    if (!block_reserve(&DEFAULT_ALLOCATOR, &string.block, 1, sizeof(char))) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory growing a string");
        return;
    }

    ((char *)string.block.data)[string.block.length] = (char)character;
    string.block.length++;

    memcpy(args_pointer(args, 0), &string, sizeof(string));
}

static void string_append(Args *args) {
    GabStringValue string = args_string_at(args, 0);
    GabStrRef other = args_string(args, 1);

    if (other.length == 0) {
        return;
    }

    char *copy = DEFAULT_ALLOCATOR.alloc(DEFAULT_ALLOCATOR.ctx, (size_t)other.length);

    if (!copy) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory appending to a string");
        return;
    }

    memcpy(copy, other.data, (size_t)other.length);

    if (block_reserve(&DEFAULT_ALLOCATOR, &string.block, other.length, sizeof(char))) {
        memcpy((char *)string.block.data + string.block.length, copy, (size_t)other.length);
        string.block.length += other.length;

        memcpy(args_pointer(args, 0), &string, sizeof(string));
    } else {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory appending to a string");
    }

    DEFAULT_ALLOCATOR.free(DEFAULT_ALLOCATOR.ctx, copy, (size_t)other.length);
}

static void string_clone(Args *args) {
    GabStringValue string = args_string_at(args, 0);

    args_return_string_copy(args, string.block.data, string.block.length);
}

static void string_from(Args *args) {
    GabStrRef string = args_string(args, 0);

    args_return_string_copy(args, string.data, string.length);
}

static const char PRELUDE_SRC[] = "module " GAB_PRELUDE_MODULE ";\n"
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

static void string_register_str(VM *vm) {
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

    library_declare_source(&prelude, PRELUDE_SRC);
}

void std_register_string(VM *vm) {
    GabLibrary std = library_open(vm, GAB_STD_MODULE, false);

    TypeRegistry *registry = vm->env.global_scope.type_registry;

    const TypeFieldSpec fields[] = {
        {
            .name = string_from_cstr(&vm->env.strings, "data"),
            .type = type_registry_block_of(registry, type_registry_get_primitive(registry, TYPE_BYTE)),
        },
    };

    const LentPart characters_named_by[] = {
        {.offset = offsetof(GabStringValue, block) + offsetof(GabBlockValue, data), .size = sizeof(void *)},
        {.offset = offsetof(GabStringValue, block) + offsetof(GabBlockValue, length),
         .size = sizeof(int32_t)},
    };

    const LibraryTypeSpec spec = {
        .name = "String",
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(*fields),
        .derefs_to = type_registry_get_primitive(registry, TYPE_STR),
        .lent_parts = characters_named_by,
        .lent_part_count = sizeof(characters_named_by) / sizeof(*characters_named_by),
    };

    const Type *string_type = type_registry_apply(registry, library_type(&std, &spec), NULL, 0);

    const Type *str_type = type_registry_ref_to(registry, type_registry_get_primitive(registry, TYPE_STR));

    const Type *ref_string = type_registry_ref_to(registry, string_type);

    const Type *int_type = type_registry_get_primitive(registry, TYPE_INT);
    const Type *const string_param[] = {str_type};

    string_register_str(vm);

    library_static(&std, string_type, "from", string_from, string_type, string_param, 1);

    const Type *const char_param[] = {int_type};

    library_method(&std, string_type, ref_string, "push", string_push, NULL, char_param, 1);
    library_method(&std, string_type, ref_string, "append", string_append, NULL, string_param, 1);

    library_method(&std, string_type, ref_string, "clone", string_clone, string_type, NULL, 0);
}
