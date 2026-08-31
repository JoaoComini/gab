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

/* Where an arena's cursor stood, so scratch work done after it can be rewound without a whole reset. */
typedef struct {
    ArenaBlock *block;
    size_t offset;
} ArenaCheckpoint;

Arena *arena_create(size_t capacity);
void *arena_alloc(Arena *arena, size_t size);
void arena_reset(Arena *arena);
void arena_destroy(Arena *arena);

ArenaCheckpoint arena_checkpoint(const Arena *arena);
void arena_rewind(Arena *arena, ArenaCheckpoint checkpoint);

Allocator arena_allocator(Arena *arena);

#endif
