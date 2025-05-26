#ifndef GAB_STRING_POOL_H
#define GAB_STRING_POOL_H

#include "string/string.h"
#include "util/hash.h"
#include "util/hash_map.h"

#include <string.h>

#define string_pool_hash(key) hash_dj2b_cstr(key.data, key.length)
#define string_pool_key_equals(key, other)                                                                   \
    (key.length == other.length) && (memcmp(key.data, other.data, key.length) == 0)
#define string_pool_key_dup(key) key
#define string_pool_entry_free(key, value) string_destroy(value);

typedef struct {
    const char *data; // Shared with String
    size_t length;
} StringKey;

GAB_HASH_MAP(StringPool, string_pool, StringKey, String *)

#endif
