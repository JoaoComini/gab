#ifndef GAB_HASH_H
#define GAB_HASH_H

#include <stddef.h>

static inline size_t hash_dj2b_cstr(const char *str, size_t length) {
    size_t hash = 5381;

    for (size_t i = 0; i < length; i++) {
        int ch = str[i];
        hash = ((hash << 5) + hash) + ch;
    }

    return hash;
}
#endif
