#ifndef GAB_ARENA_H
#define GAB_ARENA_H

#include "allocator.h"
#include <stddef.h>

typedef struct ArenaBlock {
    void *memory;
    size_t capacity;
    size_t offset;

    struct ArenaBlock *next;
} ArenaBlock;

typedef struct Arena {
    ArenaBlock *first_block;
    ArenaBlock *current_block;

    size_t block_size;
} Arena;

Arena *arena_create(size_t capacity);
void *arena_alloc(Arena *arena, size_t size);
void arena_reset(Arena *arena);
void arena_destroy(Arena *arena);

Allocator arena_allocator(Arena *arena);

#endif
