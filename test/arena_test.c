#include "arena.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

    for (int i = 0; i < 16; i++) {
        void *p = arena_alloc(arena, 1 + (size_t)i);

        assert(((uintptr_t)p & 7u) == 0);
    }

    arena_destroy(arena);
}

static void test_allocation_larger_than_a_block() {
    Arena *arena = arena_create(TEST_BLOCK_SIZE);

    unsigned char *p = arena_alloc(arena, TEST_BLOCK_SIZE * 4);
    assert(p);

    memset(p, 0xAB, TEST_BLOCK_SIZE * 4);
    assert(p[0] == 0xAB && p[TEST_BLOCK_SIZE * 4 - 1] == 0xAB);

    arena_destroy(arena);
}

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

    arena_destroy(arena);
}

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

static void test_an_oversized_allocation_leaves_the_current_block_fillable() {
    Arena *arena = arena_create(TEST_BLOCK_SIZE);

    arena_alloc(arena, 8);

    unsigned char *big = arena_alloc(arena, TEST_BLOCK_SIZE * 4);
    memset(big, 0xEF, TEST_BLOCK_SIZE * 4);

    /* The first block still had room, so what follows the oversized block belongs in it. */
    unsigned char *after = arena_alloc(arena, 8);

    assert(after > (unsigned char *)arena->first_block->memory);
    assert(after < (unsigned char *)arena->first_block->memory + TEST_BLOCK_SIZE);

    assert(big[0] == 0xEF && big[TEST_BLOCK_SIZE * 4 - 1] == 0xEF);

    arena_destroy(arena);
}

static void test_reset_reuses_blocks_after_an_oversized_one() {
    Arena *arena = arena_create(TEST_BLOCK_SIZE);

    for (int round = 0; round < 3; round++) {
        arena_alloc(arena, 8);
        arena_alloc(arena, TEST_BLOCK_SIZE * 4);

        for (int i = 0; i < 8; i++) {
            arena_alloc(arena, 40);
        }

        if (round == 0) {
            continue;
        }

        arena_reset(arena);
    }

    size_t settled = block_count(arena);

    arena_reset(arena);

    arena_alloc(arena, 8);
    arena_alloc(arena, TEST_BLOCK_SIZE * 4);

    for (int i = 0; i < 8; i++) {
        arena_alloc(arena, 40);
    }

    assert(block_count(arena) == settled);

    arena_destroy(arena);
}

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
    test_an_oversized_allocation_leaves_the_current_block_fillable();
    test_reset_reuses_blocks_after_an_oversized_one();
    test_reset_hands_back_the_same_space();

    printf("All arena tests passed\n");
    return 0;
}
