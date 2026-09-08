#ifndef GAB_CONSTANT_H
#define GAB_CONSTANT_H

#include "string/string.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Type Type;

/* A value known where it is written, which its type says how to read. Text is the string the unit
 * already holds, so a backend names it however it names any other; nothing here knows about slots.
 *
 * This is what a constant is everywhere it is meant rather than spelled: in an instruction, in an
 * operand, and as the argument a generic takes. Only the source's own spelling is its own type. */
typedef struct {
    const Type *type;

    union {
        int64_t as_int;
        float as_float;
        bool as_bool;
        String *as_string;
    };
} Constant;

static inline Constant constant_int(const Type *type, int64_t value) {
    return (Constant){.type = type, .as_int = value};
}

static inline Constant constant_float(const Type *type, float value) {
    return (Constant){.type = type, .as_float = value};
}

static inline Constant constant_bool(const Type *type, bool value) {
    return (Constant){.type = type, .as_bool = value};
}

static inline Constant constant_string(const Type *type, String *text) {
    return (Constant){.type = type, .as_string = text};
}

bool constant_is_int(Constant constant);
bool constant_is_float(Constant constant);
bool constant_is_bool(Constant constant);
bool constant_is_string(Constant constant);

/* Whether two constants are the same value; a generic interned under one must match the other. */
bool constant_equals(Constant a, Constant b);

/* A constant's value as a hash, so a type interned under one is found again. */
size_t constant_hash(Constant constant);

#endif
