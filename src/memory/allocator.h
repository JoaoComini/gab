#ifndef GAB_ALLOCATOR_H
#define GAB_ALLOCATOR_H

#include <stddef.h>

typedef struct Allocator {
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr, size_t size);

    void *ctx;
} Allocator;

extern const Allocator DEFAULT_ALLOCATOR;

#endif
