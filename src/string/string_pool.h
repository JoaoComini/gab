#ifndef GAB_STRING_POOL_H
#define GAB_STRING_POOL_H

#include "string/string.h"
#include "util/hash_map.h"

#define STRING_POOL_INITIAL_CAPACITY 128

#define string_pool_hash(key) key
#define string_pool_key_equals(key, other) key == other
#define string_pool_key_dup(key) key
#define string_pool_entry_free(key, value) free(value.data);

GAB_HASH_MAP(StringPool, string_pool, size_t, String)

#endif
