#include "allocator.h"

#include <stdlib.h>

static void *default_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void default_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

const Allocator DEFAULT_ALLOCATOR = {
    .alloc = &default_alloc,
    .free = &default_free,
    .ctx = NULL,
};
