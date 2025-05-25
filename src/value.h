#ifndef GAB_VALUE_H
#define GAB_VALUE_H

#include "type.h"
#include <stdbool.h>

typedef struct {
    TypeKind type;
    union {
        int as_int;
        float as_float;
    };
} Value;

#endif
