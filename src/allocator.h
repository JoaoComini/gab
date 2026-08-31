#ifndef GAB_ALLOCATOR_H
#define GAB_ALLOCATOR_H

#include <stddef.h>

/* 'free' takes the size the allocation was made with, or 0 where the caller does not know it: an allocator
 * that needs the size to reclaim a block can use it, and one backed by malloc ignores it. */
typedef struct Allocator {
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr, size_t size);

    void *ctx;
} Allocator;

extern const Allocator DEFAULT_ALLOCATOR;

#endif
