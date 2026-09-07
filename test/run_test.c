#include "support/emit.h"

#include "llvm/llvm_emit.h"

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the object, the driver and the binary for one file are built. */
#ifndef GAB_TEST_SCRATCH
#define GAB_TEST_SCRATCH "."
#endif

/* The emitted object calls into the runtime for what it cannot embed, such as an allocation. */
#ifndef GAB_TEST_RUNTIME
#define GAB_TEST_RUNTIME ""
#endif

#ifndef GAB_TEST_LINK_FLAGS
#define GAB_TEST_LINK_FLAGS ""
#endif

static char core_object[512];

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

/* Rewrites each 'assert(e)' to 'assert_at(e, n)', so a failure names the line in the test file that
 * wrote the check rather than the offset of a concatenated source. */
static char *number_the_checks(const char *body) {
    size_t capacity = strlen(body) * 2 + 64;
    char *out = malloc(capacity);
    size_t length = 0;

    int line = 1;

    for (const char *at = body; *at;) {
        if (strncmp(at, "assert(", 7) == 0 && (at == body || !isalnum((unsigned char)at[-1]))) {
            const char *open = at + 6;
            const char *close = open;

            int depth = 0;

            do {
                if (*close == '(') {
                    depth++;
                } else if (*close == ')') {
                    depth--;
                }

                close++;
            } while (depth > 0 && *close);

            length += (size_t)snprintf(out + length, capacity - length, "assert_at(%.*s, %d)",
                                       (int)(close - open - 2), open + 1, line);

            at = close;

            continue;
        }

        if (*at == '\n') {
            line++;
        }

        out[length++] = *at++;
    }

    out[length] = '\0';

    return out;
}

static int run_file(const char *directory, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.gab", directory, name);

    char prelude_path[512];
    snprintf(prelude_path, sizeof(prelude_path), "%s/prelude.gab", directory);

    char *prelude = read_file(prelude_path);
    char *body = read_file(path);

    if (!prelude || !body) {
        fprintf(stderr, "%s: no such test\n", path);

        free(prelude);
        free(body);

        return 1;
    }

    char *checks = number_the_checks(body);

    char *source = malloc(strlen(prelude) + strlen(checks) + 2);
    sprintf(source, "%s\n%s", prelude, checks);

    MIRFunction *bodies[64];
    size_t count = 0;

    TestEmission emission = test_lower_with_core(source, bodies, 64, &count, false);

    Arena *arena = arena_create(1 << 16);
    LLVMUnit *unit = llvm_unit_open(arena);

    for (size_t i = 0; i < count; i++) {
        llvm_unit_add(unit, bodies[i]);
    }

    char object[512];
    snprintf(object, sizeof(object), "%s/%s.o", GAB_TEST_SCRATCH, name);

    const char *error = NULL;

    int failed = 0;

    if (!llvm_unit_write_object(unit, object, &error)) {
        fprintf(stderr, "%s: %s\n", name, error);
        failed = 1;
    }

    char driver[512];
    snprintf(driver, sizeof(driver), "%s/%s_driver.c", GAB_TEST_SCRATCH, name);

    FILE *out = fopen(driver, "w");
    fprintf(out,
            "#include <stdio.h>\n"
            "extern int gab_run(void) __asm__(\"test.run\");\n"
            "static int failures, passes;\n"
            "void gab_check_passed(void){ passes++; }\n"
            "void gab_check_failed(int line){ failures++;"
            " fprintf(stderr, \"%s:%%d: check failed\\n\", line); }\n"
            "int main(void){ int r = gab_run();\n"
            " if (!passes && !failures) { fprintf(stderr, \"%s: no checks ran\\n\"); return 1; }\n"
            " if (failures) { fprintf(stderr, \"%s: %%d of %%d checks failed\\n\","
            " failures, failures + passes); return 1; }\n"
            " return r; }\n",
            path, path, path);
    fclose(out);

    char binary[512];
    snprintf(binary, sizeof(binary), "%s/%s_test", GAB_TEST_SCRATCH, name);

    char command[2048];
    snprintf(command, sizeof(command), "clang %s %s %s %s %s -o %s 2>&1", object, core_object, driver,
             GAB_TEST_RUNTIME, GAB_TEST_LINK_FLAGS, binary);

    if (!failed && system(command) != 0) {
        fprintf(stderr, "%s: the object did not link\n", name);
        failed = 1;
    }

    /* A case marked '// traps' must abort on a check the compiler emitted, rather than run to the end. */
    bool must_trap = strstr(body, "\n// traps\n") != NULL;

    if (!failed) {
        int status = system(binary);

        if (must_trap && status == 0) {
            fprintf(stderr, "%s: ran to completion, and was expected to trap\n", path);
            failed = 1;
        } else if (!must_trap && status != 0) {
            failed = 1;
        }
    }

    llvm_unit_close(unit);

    test_emission_free(&emission);

    arena_destroy(arena);

    free(source);
    free(checks);
    free(prelude);
    free(body);

    if (failed) {
        return 1;
    }

    printf("%s ok\n", name);

    return 0;
}

/* The core is compiled once and linked into every case, rather than emitted into each of them. */
static int write_core(char *path, size_t capacity) {
    char object[512];
    snprintf(object, sizeof(object), "%s/core.o", GAB_TEST_SCRATCH);
    snprintf(path, capacity, "%s/libcore.a", GAB_TEST_SCRATCH);

    MIRFunction *bodies[64];
    size_t count = 0;

    TestEmission emission = test_lower_with_core(NULL, bodies, 64, &count, true);

    Arena *arena = arena_create(1 << 16);
    LLVMUnit *unit = llvm_unit_open(arena);

    for (size_t i = 0; i < count; i++) {
        llvm_unit_add(unit, bodies[i]);
    }

    const char *error = NULL;
    int failed = !llvm_unit_write_object(unit, object, &error);

    if (failed) {
        fprintf(stderr, "core: %s\n", error);
    }

    llvm_unit_close(unit);
    test_emission_free(&emission);
    arena_destroy(arena);

    if (failed) {
        return failed;
    }

    char command[2048];
    snprintf(command, sizeof(command), "rm -f %s && ar rcs %s %s", path, path, object);

    return system(command) != 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: run_test <directory> <name>...\n");
        return 2;
    }

    if (write_core(core_object, sizeof(core_object))) {
        return 1;
    }

    int failures = 0;

    for (int i = 2; i < argc; i++) {
        failures += run_file(argv[1], argv[i]);
    }

    return failures ? 1 : 0;
}
