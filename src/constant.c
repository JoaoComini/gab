#include "constant.h"

#include "type/type.h"

#include <string.h>

bool constant_is_int(Constant constant) { return constant.type && type_kind(constant.type) == TYPE_INT; }

bool constant_is_float(Constant constant) { return constant.type && type_kind(constant.type) == TYPE_FLOAT; }

bool constant_is_bool(Constant constant) { return constant.type && type_kind(constant.type) == TYPE_BOOL; }

bool constant_is_string(Constant constant) { return constant.type && type_kind(constant.type) == TYPE_STR; }

/* The type says which member is live, so only that one is compared; the rest of the union is stale. */
bool constant_equals(Constant a, Constant b) {
    if (a.type != b.type) {
        return false;
    }

    if (constant_is_string(a)) {
        return a.as_string == b.as_string;
    }

    if (constant_is_float(a)) {
        return a.as_float == b.as_float;
    }

    if (constant_is_bool(a)) {
        return a.as_bool == b.as_bool;
    }

    return a.as_int == b.as_int;
}

size_t constant_hash(Constant constant) {
    if (constant_is_string(constant)) {
        return (size_t)(uintptr_t)constant.as_string;
    }

    if (constant_is_float(constant)) {
        uint32_t bits;

        memcpy(&bits, &constant.as_float, sizeof(bits));

        return bits;
    }

    return (size_t)(uint32_t)constant.as_int;
}
