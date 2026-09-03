#include "std.h"

#include "gab.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GabBlock block;
} String;

static String string_load(GabCtx *ctx) {
    String string;
    memcpy(&string, gab_ctx_self(ctx), sizeof(string));

    return string;
}

static void string_store(GabCtx *ctx, const String *string) {
    memcpy(gab_ctx_self(ctx), string, sizeof(*string));
}

static void string_push(GabCtx *ctx) {
    String string = string_load(ctx);
    int32_t character = gab_ctx_int(ctx, 1);

    if (!gab_block_reserve(ctx, &string.block, 1, sizeof(char))) {
        gab_ctx_fail(ctx, GAB_FAIL_OUT_OF_MEMORY, "out of memory growing a string");
        return;
    }

    ((char *)string.block.data)[string.block.length] = (char)character;
    string.block.length++;

    string_store(ctx, &string);
}

static void string_append(GabCtx *ctx) {
    String string = string_load(ctx);

    int32_t length = 0;
    const char *other = gab_ctx_string(ctx, 1, &length);

    if (length == 0) {
        return;
    }

    /* 'other' may name this string's own bytes, which the reserve is free to move. */
    char *copy = malloc((size_t)length);

    if (!copy) {
        gab_ctx_fail(ctx, GAB_FAIL_OUT_OF_MEMORY, "out of memory appending to a string");
        return;
    }

    memcpy(copy, other, (size_t)length);

    if (gab_block_reserve(ctx, &string.block, length, sizeof(char))) {
        memcpy((char *)string.block.data + string.block.length, copy, (size_t)length);
        string.block.length += length;

        string_store(ctx, &string);
    } else {
        gab_ctx_fail(ctx, GAB_FAIL_OUT_OF_MEMORY, "out of memory appending to a string");
    }

    free(copy);
}

static void string_clone(GabCtx *ctx) {
    String string = string_load(ctx);

    gab_ctx_return_string(ctx, string.block.data, string.block.length);
}

static void string_from(GabCtx *ctx) {
    int32_t length = 0;
    const char *text = gab_ctx_string(ctx, 0, &length);

    gab_ctx_return_string(ctx, text, length);
}

static const char STRING_SRC[] = "impl String {\n"
                                 "    extern func from(text: &str): String;\n"
                                 "    extern func push(self: &String, character: int);\n"
                                 "    extern func append(self: &String, other: &str);\n"
                                 "    extern func clone(self: &String): String;\n"
                                 "}\n";

void std_register_string(GabVM *vm) {
    GabError err;

    GabLib *std = gab_lib_open(vm, "std", &err);
    assert(std && "the standard library opens");

    const GabFieldSpec fields[] = {
        {"data", gab_lib_block_of(std, gab_lib_primitive(std, GAB_TYPE_BYTE))},
    };

    const GabLentPart characters_named_by[] = {
        {offsetof(String, block) + offsetof(GabBlock, data), sizeof(void *)},
        {offsetof(String, block) + offsetof(GabBlock, length), sizeof(int32_t)},
    };

    const GabTypeSpec spec = {
        .name = "String",
        .fields = fields,
        .field_count = sizeof(fields) / sizeof(*fields),
        .derefs_to = gab_lib_primitive(std, GAB_TYPE_STR),
        .lends = characters_named_by,
        .lend_count = sizeof(characters_named_by) / sizeof(*characters_named_by),
    };

    const GabType *declared = gab_lib_type(std, &spec, &err);

    assert(declared && "String declares");
    (void)declared;

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
        bool bound = gab_lib_bind(std, "String", METHODS[i].name, METHODS[i].body, &err);

        assert(bound && "a library binds each of its externs once");
        (void)bound;
    }

    bool loaded = gab_lib_source(std, STRING_SRC, &err);

    assert(loaded && "a library's declarations compile");
    (void)loaded;

    gab_lib_close(std);
}
