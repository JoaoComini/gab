#include "support/run.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A case ends in a block naming every error it must raise, in the order the compiler reports them:
 *
 *     // error
 *     // :3:1: type error: a top-level variable may not own
 *
 * The block is the whole expectation, so an error the case does not name fails it. */
#define EXPECTED_MARKER "// error"

typedef struct {
    char *text;
    size_t count;
    size_t capacity;
} Lines;

static void lines_add(Lines *lines, const char *text, size_t length) {
    if (lines->count == lines->capacity) {
        lines->capacity = lines->capacity ? lines->capacity * 2 : 8;
        lines->text = realloc(lines->text, lines->capacity * 256);
    }

    if (length > 255) {
        length = 255;
    }

    char *slot = lines->text + lines->count * 256;
    memcpy(slot, text, length);
    slot[length] = '\0';

    lines->count++;
}

static const char *lines_get(const Lines *lines, size_t index) { return lines->text + index * 256; }

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");

    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *text = malloc((size_t)size + 1);
    size_t read = fread(text, 1, (size_t)size, file);
    text[read] = '\0';

    fclose(file);

    return text;
}

static void trim(char *text) {
    size_t length = strlen(text);

    while (length && isspace((unsigned char)text[length - 1])) {
        text[--length] = '\0';
    }
}

/* Splits a case into the program to compile and the errors it claims, so the program the compiler
 * sees is the program without its expectations. */
static char *split(char *source, Lines *expected) {
    char *marker = strstr(source, EXPECTED_MARKER "\n");

    if (!marker) {
        return NULL;
    }

    for (char *at = marker; *at;) {
        char *end = strchr(at, '\n');

        if (!end) {
            break;
        }

        *end = '\0';

        char *line = at;

        while (*line == ' ' || *line == '\t') {
            line++;
        }

        if (strncmp(line, "// :", 4) == 0) {
            char *body = line + 3;
            trim(body);
            lines_add(expected, body, strlen(body));
        }

        at = end + 1;
    }

    *marker = '\0';

    return source;
}

static void collect(const char *source, Lines *actual) {
    TestContext ctx;
    test_context_init(&ctx);

    ModuleScopeMap *modules;
    Scope *scope = test_scope_with_core(&ctx, &modules);

    ASTModule *unit;
    ResolvedModule *resolved;

    MIRModule *bodies;

    /* Borrow checking runs while the bodies are built, so a lifetime error needs that pass too. */
    if (parse_module((const char *const[]){test_in_a_module(source)}, 1, NULL, ctx.arena, &ctx.strings, &unit,
                     &ctx.diagnostics) &&
        resolve_module(ctx.arena, unit, scope, modules, false, &resolved, &ctx.diagnostics)) {
        mir_build(ctx.arena, resolved, NULL, &bodies, &ctx.diagnostics);
    }

    for (size_t i = 0; i < diagnostics_count(&ctx.diagnostics); i++) {
        const Diagnostic *diagnostic = diagnostics_get(&ctx.diagnostics, i);

        char line[256];

        /* The harness prepends 'module test;', so a reported line sits one past the line in the file. */
        snprintf(line, sizeof(line), ":%d:%d: %s: %s", diagnostic->span.line - 1, diagnostic->span.column,
                 diag_kind_name(diagnostic->kind), diagnostic->message);

        lines_add(actual, line, strlen(line));
    }

    test_context_free(&ctx);
}

static int run_case(const char *directory, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.gab", directory, name);

    char *source = read_file(path);

    if (!source) {
        fprintf(stderr, "%s: no such case\n", path);

        return 1;
    }

    Lines expected = {0};

    if (!split(source, &expected)) {
        fprintf(stderr, "%s: no '%s' block, so the case names no error\n", path, EXPECTED_MARKER);

        free(source);

        return 1;
    }

    if (expected.count == 0) {
        fprintf(stderr, "%s: the '%s' block is empty, so the case states nothing\n", path, EXPECTED_MARKER);

        free(source);
        free(expected.text);

        return 1;
    }

    Lines actual = {0};
    collect(source, &actual);

    int failed = 0;

    for (size_t i = 0; i < expected.count || i < actual.count; i++) {
        const char *want = i < expected.count ? lines_get(&expected, i) : NULL;
        const char *got = i < actual.count ? lines_get(&actual, i) : NULL;

        if (want && got && strcmp(want, got) == 0) {
            continue;
        }

        if (want && !got) {
            fprintf(stderr, "%s: expected '%s', and no error was raised\n", path, want);
        } else if (got && !want) {
            fprintf(stderr, "%s: unexpected '%s'\n", path, got);
        } else {
            fprintf(stderr, "%s: expected '%s'\n%*s but was '%s'\n", path, want, (int)strlen(path), "", got);
        }

        failed = 1;
    }

    free(source);
    free(expected.text);
    free(actual.text);

    if (failed) {
        return 1;
    }

    printf("%s ok\n", name);

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: error_test <directory> <case>...\n");

        return 2;
    }

    int failures = 0;

    for (int i = 2; i < argc; i++) {
        failures += run_case(argv[1], argv[i]);
    }

    return failures ? 1 : 0;
}
