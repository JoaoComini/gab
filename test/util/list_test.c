#include "util/list.h"

#include <assert.h>

GAB_LIST(IntList, int_list, int)

typedef struct {
    size_t live;
    size_t freed_with_size;
    size_t frees;
} Counting;

static void *counting_alloc(void *ctx, size_t size) {
    Counting *c = ctx;
    c->live += size;
    return malloc(size);
}

static void counting_free(void *ctx, void *ptr, size_t size) {
    Counting *c = ctx;
    c->live -= size;
    c->frees++;
    if (size > 0) {
        c->freed_with_size++;
    }
    free(ptr);
}

static Allocator counting_allocator(Counting *c) {
    return (Allocator){.alloc = counting_alloc, .free = counting_free, .ctx = c};
}

static void test_storage_comes_from_the_allocator() {
    Counting c = {0};
    IntList list = int_list_create(counting_allocator(&c));

    int_list_add(&list, 1);
    assert(c.live > 0);

    int_list_free(&list);
    assert(c.live == 0);
}

static void test_regrowth_frees_the_old_block() {
    Counting c = {0};
    IntList list = int_list_create(counting_allocator(&c));

    for (int i = 0; i < 64; i++) {
        int_list_add(&list, i);
    }
    assert(c.frees > 0);

    for (int i = 0; i < 64; i++) {
        assert(int_list_get(&list, i) == i);
    }

    int_list_free(&list);
    assert(c.live == 0);
}

static void test_free_reports_the_size_it_was_given() {
    Counting c = {0};
    IntList list = int_list_create(counting_allocator(&c));

    int_list_add(&list, 1);
    int_list_free(&list);

    assert(c.frees == c.freed_with_size);
}

int main() {
    test_storage_comes_from_the_allocator();
    test_regrowth_frees_the_old_block();
    test_free_reports_the_size_it_was_given();

    return 0;
}
