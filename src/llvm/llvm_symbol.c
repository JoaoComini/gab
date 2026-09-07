#include "llvm/llvm_symbol.h"

#include "type/type.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char *text;
    size_t length;
    size_t capacity;

    Arena *arena;
} SymbolBuffer;

static void symbol_append(SymbolBuffer *buffer, const char *text) {
    size_t length = strlen(text);

    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity * 2 : 64;

        while (capacity < buffer->length + length + 1) {
            capacity *= 2;
        }

        char *grown = arena_alloc(buffer->arena, capacity);

        if (buffer->length) {
            memcpy(grown, buffer->text, buffer->length);
        }

        buffer->text = grown;
        buffer->capacity = capacity;
    }

    memcpy(buffer->text + buffer->length, text, length + 1);
    buffer->length += length;
}

static void append_type(SymbolBuffer *buffer, const Type *type) {
    if (!type) {
        symbol_append(buffer, "?");
        return;
    }

    switch (type_kind(type)) {
    case TYPE_REF:
        symbol_append(buffer, "ref.");
        append_type(buffer, type_pointee(type));
        return;

    case TYPE_PTR:
    case TYPE_BOX:
        symbol_append(buffer, "ptr.");
        append_type(buffer, type_pointee(type));
        return;

    case TYPE_SLICE:
        symbol_append(buffer, "slice.");
        append_type(buffer, type_slice_element(type));
        return;

    case TYPE_ARRAY: {
        char length[32];
        snprintf(length, sizeof(length), "array%d.", type_array_length(type));

        symbol_append(buffer, length);
        append_type(buffer, type_array_element(type));
        return;
    }

    default:
        break;
    }

    String *name = type_name_of(type);

    symbol_append(buffer, name ? name->data : "?");
}

static void append_type_args(SymbolBuffer *buffer, const Function *function) {
    for (size_t i = 0; i < function->type_arg_count; i++) {
        symbol_append(buffer, "$");

        if (function->type_args[i].kind == TYPE_ARG_TYPE) {
            append_type(buffer, function->type_args[i].type);
            continue;
        }

        char value[32];
        snprintf(value, sizeof(value), "%d", function->type_args[i].constant.value.as_int);

        symbol_append(buffer, value);
    }
}

const char *llvm_type_symbol(Arena *arena, const Type *type) {
    SymbolBuffer buffer = {.arena = arena};

    append_type(&buffer, type);

    return buffer.text ? buffer.text : "?";
}

const char *llvm_symbol_of(Arena *arena, const Function *function) {
    SymbolBuffer buffer = {.arena = arena};

    const FuncDecl *decl = function->decl;

    /* A foreign declaration names the symbol itself, which is the whole point of spelling an ABI. */
    if (decl->is_foreign) {
        return decl->name->data;
    }

    if (decl->module) {
        symbol_append(&buffer, decl->module->data);
        symbol_append(&buffer, ".");
    }

    if (decl->owner) {
        symbol_append(&buffer, decl->owner->data);
        append_type_args(&buffer, function);
        symbol_append(&buffer, ".");

        symbol_append(&buffer, decl->name->data);

        return buffer.text;
    }

    symbol_append(&buffer, decl->name->data);
    append_type_args(&buffer, function);

    return buffer.text;
}
