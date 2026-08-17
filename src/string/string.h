#ifndef GAB_STRING_H
#define GAB_STRING_H

#include "string/string_ref.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;    // Pointer to the string data
    size_t length; // Length of the string (excluding null terminator)
} String;

typedef struct StringPool StringPool;

// Interning: equal text always yields the same pointer within a pool, so
// String* can be compared and hashed by identity.
String *string_from_cstr(StringPool *pool, const char *str);
String *string_from_ref(StringPool *pool, StringRef ref);

#endif
