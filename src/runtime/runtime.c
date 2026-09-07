#include "runtime/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *gab_box(size_t size) {
    void *object = malloc(size);

    if (object) {
        memset(object, 0, size);
    }

    return object;
}

void gab_free(void *object) { free(object); }

void gab_trap(const char *message) {
    fputs(message, stderr);
    fputc('\n', stderr);

    abort();
}
