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
#define string_map_key_dup(key) key
#define string_map_entry_free(key, value)

typedef struct {
    const char *data; // Shared with String
    size_t length;
} StringKey;

GAB_HASH_MAP(StringMap, string_map, StringKey, String *)

// Interned strings live as long as the pool's arena, so the pool carries it and
// callers never have to know where payloads come from.
typedef struct StringPool {
    StringMap map; // buckets are malloc'd; the map resizes
    Arena *arena;  // String structs and their characters
} StringPool;

void string_pool_init(StringPool *pool, Arena *arena);

// Releases the bucket array only. Payloads go when the arena goes, so this must
// run before that arena is destroyed.
void string_pool_free(StringPool *pool);

#endif
