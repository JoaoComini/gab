#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the source, the object and the binary for one case are built. */
#ifndef GAB_TEST_SCRATCH
#define GAB_TEST_SCRATCH "."
#endif

/* The compiler each case is built with, which the suite runs as a program rather than as a library. */
#ifndef GAB_TEST_COMPILER
#define GAB_TEST_COMPILER "gabc"
#endif

/* The entry point every case links against, which counts its checks. */
#ifndef GAB_TEST_HARNESS
#define GAB_TEST_HARNESS ""
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

static int run_file(const char *directory, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.gab", directory, name);

    char *body = read_file(path);

    if (!body) {
        fprintf(stderr, "%s: no such test\n", path);

        return 1;
    }

    char binary[512];
    snprintf(binary, sizeof(binary), "%s/%s.run", GAB_TEST_SCRATCH, name);

    char command[2048];
    snprintf(command, sizeof(command), "%s -o %s %s -L %s %s", GAB_TEST_COMPILER, binary, path,
             GAB_TEST_SCRATCH, GAB_TEST_HARNESS);

    int failed = system(command) != 0;

    /* A case marked '// traps' must abort on a check the compiler emitted, rather than run to the end. */
    bool must_trap = strstr(body, "\n// traps\n") != NULL;

    if (!failed) {
        char run[1024];
        snprintf(run, sizeof(run), "%s %s", binary, path);

        int status = system(run);

        if (must_trap && status == 0) {
            fprintf(stderr, "%s: ran to completion, and was expected to trap\n", path);
            failed = 1;
        } else if (!must_trap && status != 0) {
            failed = 1;
        }
    }

    free(body);

    if (failed) {
        return 1;
    }

    printf("%s ok\n", name);

    return 0;
}

/* The checks every case calls, compiled once into a module each case imports. */
static int write_prelude(const char *directory) {
    char command[2048];
    snprintf(command, sizeof(command), "%s -c -o %s/check.o %s/prelude.gab", GAB_TEST_COMPILER,
             GAB_TEST_SCRATCH, directory);

    return system(command) != 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: run_test <directory> <name>...\n");
        return 2;
    }

    if (write_prelude(argv[1])) {
        fprintf(stderr, "the prelude did not compile\n");
        return 1;
    }

    int failures = 0;

    for (int i = 2; i < argc; i++) {
        failures += run_file(argv[1], argv[i]);
    }

    return failures ? 1 : 0;
}
