#include "arena.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Small enough that a handful of allocations forces the arena to grow, which is
// where the interesting behaviour is.
#define TEST_BLOCK_SIZE 64

static size_t block_count(const Arena *arena) {
    size_t count = 0;

    for (const ArenaBlock *block = arena->first_block; block; block = block->next) {
        count++;
    }

    return count;
}

static void test_allocations_are_aligned() {
    Arena *arena = arena_create(TEST_BLOCK_SIZE);

    // Odd sizes, so every allocation after the first starts from a misaligned
    // offset unless the arena realigns it.
    for (int i = 0; i < 16; i++) {
        void *p = arena_alloc(arena, 1 + (size_t)i);

        assert(((uintptr_t)p & 7u) == 0);
    }

    arena_destroy(arena);
}

// An allocation larger than the block size still has to be served, so the block
// is sized to the request rather than to the arena's default.
static void test_allocation_larger_than_a_block() {
    Arena *arena = arena_create(TEST_BLOCK_SIZE);

    unsigned char *p = arena_alloc(arena, TEST_BLOCK_SIZE * 4);
    assert(p);

    // Writing the whole span proves the block really is that large.
    memset(p, 0xAB, TEST_BLOCK_SIZE * 4);
    assert(p[0] == 0xAB && p[TEST_BLOCK_SIZE * 4 - 1] == 0xAB);

    arena_destroy(arena);
}

// Reset rewinds the arena without releasing its blocks, so a second round of
// the same allocations reuses them instead of allocating more. Growing after a
// reset used to overwrite current_block->next, orphaning every block already
// chained past it — a leak that only appears on the second round.
static void test_reset_reuses_blocks_instead_of_leaking_them() {
    Arena *arena = arena_create(TEST_BLOCK_SIZE);

    for (int i = 0; i < 8; i++) {
        arena_alloc(arena, 40);
    }

    size_t after_first_round = block_count(arena);
    assert(after_first_round > 1 && "the allocations above must have forced growth");

    for (int round = 0; round < 3; round++) {
        arena_reset(arena);

        for (int i = 0; i < 8; i++) {
            arena_alloc(arena, 40);
        }

        assert(block_count(arena) == after_first_round);
    }

    // Whatever is still chained has to be reachable from first_block, or
    // arena_destroy cannot free it. LeakSanitizer is what actually checks this.
    arena_destroy(arena);
}

// A reset arena that then needs a block bigger than the one it recycled must
// still serve the request rather than overrun the smaller block.
static void test_reset_then_allocate_larger_than_the_recycled_block() {
    Arena *arena = arena_create(TEST_BLOCK_SIZE);

    for (int i = 0; i < 8; i++) {
        arena_alloc(arena, 40);
    }

    arena_reset(arena);

    unsigned char *p = arena_alloc(arena, TEST_BLOCK_SIZE * 8);
    assert(p);

    memset(p, 0xCD, TEST_BLOCK_SIZE * 8);
    assert(p[TEST_BLOCK_SIZE * 8 - 1] == 0xCD);

    arena_destroy(arena);
}

// Reset hands the same space out again, so the bytes a caller gets must not be
// assumed to carry anything over.
static void test_reset_hands_back_the_same_space() {
    Arena *arena = arena_create(TEST_BLOCK_SIZE);

    void *first = arena_alloc(arena, 16);
    arena_reset(arena);
    void *again = arena_alloc(arena, 16);

    assert(first == again);

    arena_destroy(arena);
}

int main(void) {
    test_allocations_are_aligned();
    test_allocation_larger_than_a_block();
    test_reset_reuses_blocks_instead_of_leaking_them();
    test_reset_then_allocate_larger_than_the_recycled_block();
    test_reset_hands_back_the_same_space();

    printf("All arena tests passed\n");
    return 0;
}
