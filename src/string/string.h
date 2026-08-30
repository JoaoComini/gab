#ifndef GAB_STRING_H
#define GAB_STRING_H

#include "string/string_ref.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t length;
} String;

typedef struct StringPool StringPool;

String *string_from_cstr(StringPool *pool, const char *str);
String *string_from_ref(StringPool *pool, StringRef ref);

#endif
