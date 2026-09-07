#include "support/emit.h"

#include "llvm/llvm_emit.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the object, the driver and the binary for one file are built. */
#ifndef GAB_TEST_SCRATCH
#define GAB_TEST_SCRATCH "."
#endif

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

/* Every body a unit declares, so a call reaches the one it names rather than a declaration. */
static void emit_unit(LLVMUnit *unit, const char *source, TestEmission *held, size_t *count) {
    static const char *NAMES[] = {"assert", "run"};

    for (size_t i = 0; i < sizeof(NAMES) / sizeof(*NAMES); i++) {
        held[*count] = test_lower_ir_named(source, NAMES[i]);

        llvm_unit_add(unit, held[*count].ir);

        (*count)++;
    }
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

    char *source = malloc(strlen(prelude) + strlen(body) + 2);
    sprintf(source, "%s\n%s", prelude, body);

    TestEmission held[8];
    size_t count = 0;

    Arena *arena = arena_create(1 << 16);
    LLVMUnit *unit = llvm_unit_open(arena);

    emit_unit(unit, source, held, &count);

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
    fprintf(out, "extern int gab_run(void) __asm__(\"test.run\");\nint main(void){ return gab_run(); }\n");
    fclose(out);

    char binary[512];
    snprintf(binary, sizeof(binary), "%s/%s_test", GAB_TEST_SCRATCH, name);

    char command[2048];
    snprintf(command, sizeof(command), "clang %s %s -o %s 2>&1", object, driver, binary);

    if (!failed && system(command) != 0) {
        fprintf(stderr, "%s: the object did not link\n", name);
        failed = 1;
    }

    if (!failed && system(binary) != 0) {
        fprintf(stderr, "%s: an assertion failed\n", name);
        failed = 1;
    }

    llvm_unit_close(unit);

    for (size_t i = 0; i < count; i++) {
        test_emission_free(&held[i]);
    }

    arena_destroy(arena);

    free(source);
    free(prelude);
    free(body);

    if (failed) {
        return 1;
    }

    printf("%s ok\n", name);

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: gab_runner <directory> <name>...\n");
        return 2;
    }

    int failures = 0;

    for (int i = 2; i < argc; i++) {
        failures += run_file(argv[1], argv[i]);
    }

    return failures ? 1 : 0;
}
