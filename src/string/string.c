#include "string.h"

#include "allocator.h"
#include "string/string.h"
#include "string/string_pool.h"
#include "string/string_ref.h"

#include <stdlib.h>
#include <string.h>

static StringPool string_pool = {0};

static void *string_pool_allocator_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void string_pool_allocator_free(void *ctx, void *ptr) {
    (void)ctx;
    StringPoolEntry *entry = ptr;

    if (entry->value) {
        string_destroy(entry->value);
    }
    free(entry);
}

static const Allocator string_pool_allocator = (Allocator){
    .alloc = &string_pool_allocator_alloc,
    .free = &string_pool_allocator_free,
    .ctx = NULL,
};

void string_init() {
    string_pool_init_alloc(&string_pool, string_pool_allocator, STRING_POOL_INITIAL_CAPACITY);
}

void string_deinit() { string_pool_free(&string_pool); }

String *string_from_cstr_len(const char *cstr, size_t length) {
    StringKey key = {.data = cstr, .length = length};

    String **saved = string_pool_lookup(&string_pool, key);

    if (saved) {
        return *saved;
    }

    String *string = malloc(sizeof(String));
    string->length = length;
    string->data = malloc(string->length);
    memcpy(string->data, cstr, string->length);

    key.data = string->data;

    return *string_pool_insert(&string_pool, key, string);
}

String *string_from_cstr(const char *cstr) { return string_from_cstr_len(cstr, strlen(cstr)); }
String *string_from_ref(StringRef ref) { return string_from_cstr_len(ref.data, ref.length); }

bool string_equals(const String *a, const String *b) {
    if (a == b) {
        return true;
    }

    if (a->length != b->length) {
        return false;
    }

    return memcmp(a->data, b->data, a->length) == 0;
}

void string_destroy(String *s) {
    free(s->data);
    free(s);
}
