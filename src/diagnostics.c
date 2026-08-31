#include "diagnostics.h"

#include <stdarg.h>
#include <string.h>

void diagnostics_init(Diagnostics *diagnostics, Arena *arena, const char *module) {
    diagnostics->arena = arena;
    diagnostics->module = module;
    diagnostics->items = diagnostic_list_create(arena_allocator(arena));
}

void diagnostics_free(Diagnostics *diagnostics) { diagnostic_list_free(&diagnostics->items); }

static char *diag_format(Arena *arena, const char *fmt, va_list args) {
    va_list measure;
    va_copy(measure, args);
    int length = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);

    if (length < 0) {
        return NULL;
    }

    char *message = arena_alloc(arena, (size_t)length + 1);
    vsnprintf(message, (size_t)length + 1, fmt, args);

    return message;
}

void diag_error(Diagnostics *diagnostics, DiagKind kind, Span span, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *message = diag_format(diagnostics->arena, fmt, args);
    va_end(args);

    Diagnostic diagnostic = {
        .kind = kind,
        .span = span,
        .message = message,
    };

    diagnostic_list_add(&diagnostics->items, diagnostic);
}

bool diagnostics_has_errors(const Diagnostics *diagnostics) { return diagnostics->items.size > 0; }

size_t diagnostics_count(const Diagnostics *diagnostics) { return diagnostics->items.size; }

const Diagnostic *diagnostics_get(const Diagnostics *diagnostics, size_t index) {
    if (index >= diagnostics->items.size) {
        return NULL;
    }

    return &diagnostics->items.data[index];
}

const char *diag_kind_name(DiagKind kind) {
    switch (kind) {
    case GAB_ERR_SYNTAX:
        return "syntax error";
    case GAB_ERR_TYPE:
        return "type error";
    case GAB_ERR_NAME:
        return "name error";
    case GAB_ERR_CODEGEN:
        return "codegen error";
    case GAB_ERR_LIFETIME:
        return "lifetime error";
    }

    return "error";
}

void diagnostics_print(const Diagnostics *diagnostics, FILE *out) {
    const char *module = diagnostics->module ? diagnostics->module : "<script>";

    for (size_t i = 0; i < diagnostics->items.size; i++) {
        const Diagnostic *diagnostic = &diagnostics->items.data[i];

        fprintf(out, "%s:%d:%d: %s: %s\n", module, diagnostic->span.line, diagnostic->span.column,
                diag_kind_name(diagnostic->kind), diagnostic->message);
    }
}
