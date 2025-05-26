#ifndef GAB_STRING_H
#define GAB_STRING_H

#include "string/string_ref.h"

#include <stdbool.h>
#include <stddef.h>

#define STRING_POOL_INITIAL_CAPACITY 128

typedef struct {
    char *data;    // Pointer to the string data
    size_t length; // Length of the string (excluding null terminator)
} String;

void string_init();
void string_deinit();

String *string_from_cstr(const char *str);
String *string_from_ref(StringRef ref);

void string_destroy(String *s);

bool string_equals(const String *a, const String *b);
#endif
