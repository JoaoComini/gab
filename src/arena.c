#include "arena.h"
#include "allocator.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static ArenaBlock *arena_block_create(size_t size) {
    ArenaBlock *block = malloc(sizeof(ArenaBlock) + size);
    assert(block && "failed to allocate arena memory");
    block->memory = block + 1;
    block->next = NULL;
    block->capacity = size;
    block->offset = 0;
    return block;
}

static void arena_grow(Arena *arena, size_t size) {
    size_t block_size = arena->block_size > size ? arena->block_size : size * 2;
    ArenaBlock *block = arena_block_create(block_size);
    arena->current_block->next = block;
    arena->current_block = block;
}

Arena *arena_create(size_t block_size) {
    Arena *arena = malloc(sizeof(Arena));
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

static size_t align_up(size_t offset, size_t alignment) {
    return (offset + (alignment - 1)) & ~(alignment - 1);
}

void *arena_alloc(Arena *arena, size_t size) {
    ArenaBlock *block = arena->current_block;

    size_t offset = align_up(block->offset, 8);
    if (offset + size > block->capacity) {
        arena_grow(arena, size);

        block = arena->current_block;
        offset = 0;
    }

    void *ptr = block->memory + offset;
    block->offset = offset + size;
    return ptr;
}

void arena_reset(Arena *arena) {
    ArenaBlock *block = arena->first_block;
    while (block) {
        block->offset = 0;
        block = block->next;
    }

    // reset current pointer to the first block
    arena->current_block = arena->first_block;
}

static void *arena_allocator_alloc(void *ctx, size_t size) { return arena_alloc((Arena *)ctx, size); }

static void arena_allocator_free(void *ctx, void *ptr) { (void)ctx; }

Allocator arena_allocator(Arena *arena) {
    return (Allocator){.alloc = &arena_allocator_alloc, .free = &arena_allocator_free, .ctx = arena};
}
