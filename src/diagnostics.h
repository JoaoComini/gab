#ifndef GAB_DIAGNOSTICS_H
#define GAB_DIAGNOSTICS_H

#include "arena.h"
#include "util/list.h"

#include <stdbool.h>
#include <stdio.h>

typedef enum {
    GAB_ERR_SYNTAX,
    GAB_ERR_TYPE,
    GAB_ERR_NAME,
    GAB_ERR_CODEGEN,
    GAB_ERR_LIFETIME,
} DiagKind;

typedef struct {
    int line;
    int column;
} Span;

typedef struct {
    DiagKind kind;
    Span span;
    char *message;
} Diagnostic;

#define diagnostic_list_item_free(item) (void)(item)
GAB_LIST(DiagnosticList, diagnostic_list, Diagnostic)

typedef struct {
    Arena *arena;
    const char *module;
    DiagnosticList items;
} Diagnostics;

void diagnostics_init(Diagnostics *diagnostics, Arena *arena, const char *module);
void diagnostics_free(Diagnostics *diagnostics);

void diag_error(Diagnostics *diagnostics, DiagKind kind, Span span, const char *fmt, ...);

bool diagnostics_has_errors(const Diagnostics *diagnostics);
size_t diagnostics_count(const Diagnostics *diagnostics);

const Diagnostic *diagnostics_get(const Diagnostics *diagnostics, size_t index);

const char *diag_kind_name(DiagKind kind);
void diagnostics_print(const Diagnostics *diagnostics, FILE *out);

#endif
