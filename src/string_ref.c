#include "string_ref.h"
#include <stdlib.h>
#include <string.h>

char *string_ref_to_cstr(StringRef ref) {
    char *cstr = malloc(ref.length + 1);
    memcpy(cstr, ref.data, ref.length);
    cstr[ref.length] = '\0';

    return cstr;
}

bool string_ref_equals_cstr(StringRef ref, const char *cstr) {
    return strncmp(ref.data, cstr, ref.length) == 0;
}
