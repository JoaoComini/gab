#include "arena.h"
#include "string/string.h"
#include "string/string_pool.h"
#include "string/string_ref.h"
#include "support/test_context.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_interning_returns_same_pointer() {
    TestContext ctx;
    test_context_init(&ctx);

    String *first = string_from_cstr(&ctx.strings, "position");
    String *second = string_from_cstr(&ctx.strings, "position");

    assert(first == second);
    assert(first->length == 8);
    assert(strcmp(first->data, "position") == 0);

    test_context_free(&ctx);
}

static void test_distinct_text_differs() {
    TestContext ctx;
    test_context_init(&ctx);

    assert(string_from_cstr(&ctx.strings, "x") != string_from_cstr(&ctx.strings, "y"));

    assert(string_from_cstr(&ctx.strings, "pos") != string_from_cstr(&ctx.strings, "position"));

    test_context_free(&ctx);
}

static void test_from_ref_and_cstr_agree() {
    TestContext ctx;
    test_context_init(&ctx);

    StringRef ref = {.data = "positionXX", .length = 8};

    assert(string_from_ref(&ctx.strings, ref) == string_from_cstr(&ctx.strings, "position"));

    test_context_free(&ctx);
}

static void test_data_is_null_terminated() {
    TestContext ctx;
    test_context_init(&ctx);

    StringRef ref = {.data = "abcdef", .length = 3};
    String *string = string_from_ref(&ctx.strings, ref);

    assert(string->length == 3);
    assert(string->data[3] == '\0');
    assert(strcmp(string->data, "abc") == 0);

    test_context_free(&ctx);
}

static void test_pools_are_independent() {
    TestContext a;
    TestContext b;
    test_context_init(&a);
    test_context_init(&b);

    String *from_a = string_from_cstr(&a.strings, "shared");
    String *from_b = string_from_cstr(&b.strings, "shared");

    assert(from_a != from_b);
    assert(strcmp(from_a->data, from_b->data) == 0);

    test_context_free(&a);

    assert(strcmp(from_b->data, "shared") == 0);
    assert(string_from_cstr(&b.strings, "shared") == from_b);

    test_context_free(&b);
}

static void test_pointers_survive_resize() {
    TestContext ctx;
    test_context_init(&ctx);

    enum { COUNT = STRING_POOL_INITIAL_CAPACITY * 4 };

    String *interned[COUNT];
    char buffer[32];

    for (int i = 0; i < COUNT; i++) {
        snprintf(buffer, sizeof(buffer), "identifier_%d", i);
        interned[i] = string_from_cstr(&ctx.strings, buffer);
    }

    for (int i = 0; i < COUNT; i++) {
        snprintf(buffer, sizeof(buffer), "identifier_%d", i);

        String *again = string_from_cstr(&ctx.strings, buffer);

        assert(again == interned[i]);
        assert(strcmp(again->data, buffer) == 0);
    }

    test_context_free(&ctx);
}

int main(void) {
    test_interning_returns_same_pointer();
    test_distinct_text_differs();
    test_from_ref_and_cstr_agree();
    test_data_is_null_terminated();
    test_pools_are_independent();
    test_pointers_survive_resize();

    printf("All string pool tests passed\n");
    return 0;
}
