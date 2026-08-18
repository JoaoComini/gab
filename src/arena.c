#include "arena.h"
#include "allocator.h"
#include "util/align.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Every allocation is aligned to this, which covers any scalar the VM stores —
// including the 8-byte pointers a Type or a stack slot can hold.
#define ARENA_ALIGNMENT 8

// Reset does not release blocks, so a stale pointer into a reset arena still
// addresses mapped memory and reads whatever was there before. That turns a
// lifetime bug into wrong answers much later instead of a crash at the first
// bad read, so debug builds fill reclaimed space with a pattern that is both
// an obvious value in a debugger and a non-canonical address if dereferenced.
#ifndef NDEBUG
#define ARENA_POISON 0xDD
#endif

static ArenaBlock *arena_block_create(size_t size) {
    ArenaBlock *block = malloc(sizeof(ArenaBlock) + size);

    // Threading a failure path back through every arena_alloc caller would buy
    // nothing: none of them can carry on without the memory. Failing loudly
    // beats an assert that NDEBUG turns into a null dereference.
    if (!block) {
        fprintf(stderr, "gab: out of memory allocating a %zu byte arena block\n", size);
        abort();
    }

    block->memory = block + 1;
    block->next = NULL;
    block->capacity = size;
    block->offset = 0;
    return block;
}

// Appends a block after the current one, keeping any blocks a previous reset
// left further down the chain: current_block is rewound to the front by a
// reset, so overwriting its 'next' here would orphan every block after it.
static void arena_grow(Arena *arena, size_t size) {
    if (arena->current_block->next) {
        ArenaBlock *reused = arena->current_block->next;

        // A recycled block only helps if the allocation actually fits it.
        if (size <= reused->capacity) {
            arena->current_block = reused;
            return;
        }
    }

    size_t block_size = arena->block_size > size ? arena->block_size : size * 2;
    ArenaBlock *block = arena_block_create(block_size);

    // Splice rather than overwrite, so blocks past this one survive.
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
        arena_grow(arena, size);

        block = arena->current_block;

        // A fresh block starts at zero, but a recycled one is only rewound to
        // it, so the alignment still has to be applied.
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
        // Only the part that was handed out: the rest was never live.
        memset(block->memory, ARENA_POISON, block->offset);
#endif
        block->offset = 0;
        block = block->next;
    }

    // Blocks are kept for the next round rather than released, which is the
    // point of a reset; arena_grow walks back down the chain to reuse them.
    arena->current_block = arena->first_block;
}

static void *arena_allocator_alloc(void *ctx, size_t size) { return arena_alloc((Arena *)ctx, size); }

static void arena_allocator_free(void *ctx, void *ptr) {
    (void)ctx;
    (void)ptr;
}

Allocator arena_allocator(Arena *arena) {
    return (Allocator){.alloc = &arena_allocator_alloc, .free = &arena_allocator_free, .ctx = arena};
}
