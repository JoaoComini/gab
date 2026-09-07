#include <stdio.h>

/* The entry point a case links against: it runs the case's 'run' and reports which checks failed. The
 * case names itself as an argument, so one harness serves every case rather than being written per
 * case. */

extern int gab_run(void) __asm__("test.run");

static int failures;
static int passes;

static const char *what_ran = "a case";

void gab_check_passed(void) { passes++; }

void gab_check_failed(int line) {
    failures++;

    fprintf(stderr, "%s:%d: check failed\n", what_ran, line);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        what_ran = argv[1];
    }

    int status = gab_run();

    /* A case whose checks never ran passed nothing, which a silent exit would report as success. */
    if (!passes && !failures) {
        fprintf(stderr, "%s: no checks ran\n", what_ran);
        return 1;
    }

    if (failures) {
        fprintf(stderr, "%s: %d of %d checks failed\n", what_ran, failures, failures + passes);
        return 1;
    }

    return status;
}
