#include "string_ref.h"
#include <stdlib.h>
#include <string.h>

StringRef string_ref_create(const char *data) { return (StringRef){.data = data, .length = strlen(data)}; }

char *string_ref_to_cstr(StringRef ref) {
    char *cstr = malloc(ref.length + 1);
    memcpy(cstr, ref.data, ref.length);
    cstr[ref.length] = '\0';

    return cstr;
}

bool string_ref_equals_cstr(StringRef ref, const char *cstr) {
    return strlen(cstr) == ref.length && strncmp(ref.data, cstr, ref.length) == 0;
}
