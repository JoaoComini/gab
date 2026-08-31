#ifndef GAB_STRING_POOL_H
#define GAB_STRING_POOL_H

#include "arena.h"
#include "string/string.h"
#include "util/hash.h"
#include "util/hash_map.h"

#include <string.h>

#define STRING_POOL_INITIAL_CAPACITY 128

#define string_map_hash(key) hash_dj2b_cstr(key.data, key.length)
#define string_map_key_equals(key, other)                                                                    \
    (key.length == other.length) && (memcmp(key.data, other.data, key.length) == 0)

typedef struct {
    const char *data;
    size_t length;
} StringKey;

GAB_HASH_MAP(StringMap, string_map, StringKey, String *)

typedef struct StringPool {
    StringMap map;
    Arena *arena;
} StringPool;

void string_pool_init(StringPool *pool, Arena *arena);

void string_pool_free(StringPool *pool);

#endif
