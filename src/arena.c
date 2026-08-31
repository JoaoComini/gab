#include "arena.h"
#include "allocator.h"
#include "util/align.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGNMENT 8

#ifndef NDEBUG
#define ARENA_POISON 0xDD
#endif

static ArenaBlock *arena_block_create(size_t size) {
    ArenaBlock *block = malloc(sizeof(ArenaBlock) + size);

    if (!block) {
        fprintf(stderr, "gab: out of memory allocating a %zu byte arena block\n", size);
        abort();
    }

    block->memory = block + 1;
    block->next = NULL;
    block->capacity = size;
    block->offset = 0;
    block->dedicated = false;
    return block;
}

/* An allocation too big for a shared block gets one of its own, linked after the current block rather than
 * becoming it, so the space left in the current block is still handed out. Its capacity is exactly what was
 * asked for, so arena_grow must never hand it out again: it stays full until the arena resets. */
static void *arena_alloc_oversized(Arena *arena, size_t size) {
    for (ArenaBlock *reused = arena->current_block->next; reused; reused = reused->next) {
        if (reused->dedicated && reused->offset == 0 && size <= reused->capacity) {
            reused->offset = size;
            return reused->memory;
        }
    }

    ArenaBlock *block = arena_block_create(size);
    block->offset = size;
    block->dedicated = true;

    block->next = arena->current_block->next;
    arena->current_block->next = block;

    return block->memory;
}

static void arena_grow(Arena *arena, size_t size) {
    for (ArenaBlock *reused = arena->current_block->next; reused; reused = reused->next) {
        if (!reused->dedicated && size <= reused->capacity) {
            arena->current_block = reused;
            return;
        }
    }

    ArenaBlock *block = arena_block_create(arena->block_size);

    block->next = arena->current_block->next;
    arena->current_block->next = block;
    arena->current_block = block;
}

Arena *arena_create(size_t block_size) {
    Arena *arena = malloc(sizeof(Arena));

    if (!arena) {
        fprintf(stderr, "gab: out of memory allocating an arena\n");
        abort();
    }

    arena->block_size = block_size;
    arena->first_block = arena_block_create(block_size);
    arena->current_block = arena->first_block;
    return arena;
}

void arena_destroy(Arena *arena) {
    ArenaBlock *block = arena->first_block;
    while (block) {
        ArenaBlock *next = block->next;
        free(block);

        block = next;
    }
    free(arena);
}

void *arena_alloc(Arena *arena, size_t size) {
    ArenaBlock *block = arena->current_block;

    size_t offset = align_up(block->offset, ARENA_ALIGNMENT);
    if (offset + size > block->capacity) {
        if (size > arena->block_size) {
            return arena_alloc_oversized(arena, size);
        }

        arena_grow(arena, size);

        block = arena->current_block;

        offset = align_up(block->offset, ARENA_ALIGNMENT);
    }

    void *ptr = (char *)block->memory + offset;
    block->offset = offset + size;
    return ptr;
}

void arena_reset(Arena *arena) {
    ArenaBlock *block = arena->first_block;
    while (block) {
#ifdef ARENA_POISON

        memset(block->memory, ARENA_POISON, block->offset);
#endif
        block->offset = 0;
        block = block->next;
    }

    arena->current_block = arena->first_block;
}

ArenaCheckpoint arena_checkpoint(const Arena *arena) {
    return (ArenaCheckpoint){.block = arena->current_block, .offset = arena->current_block->offset};
}

/* Every block from the checkpoint's forward was touched only after it, so each rewinds to empty; the
 * checkpoint's own block rewinds to where its cursor stood, not to empty. */
void arena_rewind(Arena *arena, ArenaCheckpoint checkpoint) {
#ifdef ARENA_POISON
    memset((char *)checkpoint.block->memory + checkpoint.offset, ARENA_POISON,
           checkpoint.block->offset - checkpoint.offset);
#endif
    checkpoint.block->offset = checkpoint.offset;

    for (ArenaBlock *block = checkpoint.block->next; block; block = block->next) {
#ifdef ARENA_POISON
        memset(block->memory, ARENA_POISON, block->offset);
#endif
        block->offset = 0;
    }

    arena->current_block = checkpoint.block;
}

static void *arena_allocator_alloc(void *ctx, size_t size) { return arena_alloc((Arena *)ctx, size); }

static void arena_allocator_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)ptr;
    (void)size;
}

Allocator arena_allocator(Arena *arena) {
    return (Allocator){.alloc = &arena_allocator_alloc, .free = &arena_allocator_free, .ctx = arena};
}
