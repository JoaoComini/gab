#ifndef GAB_VARIANT_H
#define GAB_VARIANT_H

#include <stdbool.h>
typedef enum {
    VARIANT_NUMBER,
    VARIANT_BOOL,
} VariantType;

typedef struct {
    VariantType type;
    union {
        float number_var;
        bool bool_var;
    };
} Variant;

#endif
