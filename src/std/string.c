#include "library.h"
#include "std/std.h"

#include "allocator.h"
#include "gab.h"
#include "object.h"
#include "scope.h"
#include "string/string.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void string_push(GabCtx *ctx, StringValue *self, int32_t character) {
    if (!block_reserve(&DEFAULT_ALLOCATOR, &self->block, 1, sizeof(char))) {
        gab_ctx_fail(ctx, "out of memory growing a string");
        return;
    }

    ((char *)self->block.data)[self->block.length] = (char)character;
    self->block.length++;
}

static void string_append(GabCtx *ctx, StringValue *self, StrRef other) {
    if (other.length == 0) {
        return;
    }

    char *copy = DEFAULT_ALLOCATOR.alloc(DEFAULT_ALLOCATOR.ctx, (size_t)other.length);

    if (!copy) {
        gab_ctx_fail(ctx, "out of memory appending to a string");
        return;
    }

    memcpy(copy, other.data, (size_t)other.length);

    if (block_reserve(&DEFAULT_ALLOCATOR, &self->block, other.length, sizeof(char))) {
        memcpy((char *)self->block.data + self->block.length, copy, (size_t)other.length);
        self->block.length += other.length;
    } else {
        gab_ctx_fail(ctx, "out of memory appending to a string");
    }

    DEFAULT_ALLOCATOR.free(DEFAULT_ALLOCATOR.ctx, copy, (size_t)other.length);
}

static GabStr string_clone(GabCtx *ctx, const StringValue *self) {
    return gab_str_copy(ctx, self->block.data, self->block.length);
}

static GabStr string_from(GabCtx *ctx, StrRef text) { return gab_str_copy(ctx, text.data, text.length); }

static const char STD_SRC[] = "module " GAB_STD_MODULE ";\n"
                              "impl String {\n"
                              "    extern func from(text: &str): String;\n"
                              "    extern func push(self: &String, character: int);\n"
                              "    extern func append(self: &String, other: &str);\n"
                              "    extern func clone(self: &String): String;\n"
                              "}\n";

void std_register_string(VM *vm) {
    Library std = library_open(vm, GAB_STD_MODULE, false);

    TypeRegistry *registry = vm->env.global_scope.type_registry;

    const TypeFieldSpec fields[] = {
        {
            .name = string_from_cstr(&vm->env.strings, "data"),
            .type = type_registry_block_of(registry, type_registry_get_primitive(registry, TYPE_BYTE)),
        },
    };

    const LentPart characters_named_by[] = {
        {.offset = offsetof(StringValue, block) + offsetof(BlockValue, data), .size = sizeof(void *)},
        {.offset = offsetof(StringValue, block) + offsetof(BlockValue, length), .size = sizeof(int32_t)},
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
        GabExternFn symbol;
    } METHODS[] = {
        {"from", (GabExternFn)string_from},
        {"push", (GabExternFn)string_push},
        {"append", (GabExternFn)string_append},
        {"clone", (GabExternFn)string_clone},
    };

    for (size_t i = 0; i < sizeof(METHODS) / sizeof(*METHODS); i++) {
        library_extern(&std, "String", METHODS[i].name, METHODS[i].symbol);
    }

    library_declare_source(&std, STD_SRC);
}
