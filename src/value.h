#ifndef GAB_VALUE_H
#define GAB_VALUE_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

// A slot carries no tag: static types already say which member is live, and an
// untagged 4-byte slot is what lets a struct spread over consecutive slots be
// byte-identical to the equivalent C struct.
typedef union {
    int32_t as_int;
    float as_float;
} Value;

_Static_assert(sizeof(Value) == 4, "a slot must be 4 bytes for structs to tile over consecutive slots");

#endif
