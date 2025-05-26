#include "string.h"

#include "util/hash.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "string/string_pool.h"

#include <stdlib.h>
#include <string.h>

static StringPool string_pool = {0};

void string_init() { string_pool_init(&string_pool, STRING_POOL_INITIAL_CAPACITY); }
void string_deinit() { string_pool_free(&string_pool); }

String *string_from_cstr_len(const char *cstr, size_t length) {
    size_t hash = hash_dj2b_cstr(cstr, length);

    String *saved = string_pool_lookup(&string_pool, hash);

    if (saved) {
        return saved;
    }

    String string;
    string.length = length;
    string.data = malloc(string.length);
    string.hash = hash;
    memcpy(string.data, cstr, string.length);

    return string_pool_insert(&string_pool, hash, string);
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

    if (a->hash != b->hash) {
        return false;
    }

    return memcmp(a->data, b->data, a->length) == 0;
}
