#include "string.h"

#include "arena.h"
#include "string/string_pool.h"
#include "string/string_ref.h"

#include <string.h>

void string_pool_init(StringPool *pool, Arena *arena) {
    string_map_init(&pool->map, STRING_POOL_INITIAL_CAPACITY);
    pool->arena = arena;
}

void string_pool_free(StringPool *pool) { string_map_free(&pool->map); }

static String *string_from_cstr_len(StringPool *pool, const char *cstr, size_t length) {
    StringKey key = {.data = cstr, .length = length};

    String **saved = string_map_lookup(&pool->map, key);

    if (saved) {
        return *saved;
    }

    String *string = arena_alloc(pool->arena, sizeof(String));
    string->length = length;

    string->data = arena_alloc(pool->arena, length + 1);

    if (length) {
        memcpy(string->data, cstr, length);
    }
    string->data[length] = '\0';

    key.data = string->data;

    return *string_map_insert(&pool->map, key, string);
}

String *string_from_cstr(StringPool *pool, const char *cstr) {
    return string_from_cstr_len(pool, cstr, strlen(cstr));
}

String *string_from_ref(StringPool *pool, StringRef ref) {
    return string_from_cstr_len(pool, ref.data, ref.length);
}
