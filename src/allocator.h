#ifndef GAB_ALLOCATOR_H
#define GAB_ALLOCATOR_H

#include <stddef.h>
#include <stdlib.h>

typedef struct Allocator {
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr);
    void *ctx;
} Allocator;

static void *default_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void default_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static const Allocator DEFAULT_ALLOCATOR = {
    .alloc = &default_alloc,
    .free = &default_free,
    .ctx = NULL,
};

#endif
