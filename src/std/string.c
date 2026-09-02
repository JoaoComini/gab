#include "library.h"
#include "std/std.h"

#include "vm/args.h"
#include "vm/interp.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

static const char STD_SRC[] = "module " GAB_STD_MODULE ";\n"
                              "impl String {\n"
                              "    extern func from(text: &str): String;\n"
                              "    extern func push(self: &String, character: int);\n"
                              "    extern func append(self: &String, other: &str);\n"
                              "    extern func clone(self: &String): String;\n"
                              "}\n";

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

    (void)type_registry_apply(registry, library_type(&std, &spec), NULL, 0);

    static const struct {
        const char *name;
        GabExternFn body;
    } METHODS[] = {
        {"from", string_from},
        {"push", string_push},
        {"append", string_append},
        {"clone", string_clone},
    };

    for (size_t i = 0; i < sizeof(METHODS) / sizeof(*METHODS); i++) {
        library_extern(&std, "String", METHODS[i].name, METHODS[i].body);
    }

    library_declare_source(&std, STD_SRC);
}
