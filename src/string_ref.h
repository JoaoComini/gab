#ifndef GAB_STRING_REF_H
#define GAB_STRING_REF_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *data;
    size_t length;
} StringRef;

char *string_ref_to_cstr(StringRef ref);
bool string_ref_equals_cstr(StringRef ref, const char *cstr);

#endif
