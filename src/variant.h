#ifndef GAB_VARIANT_H
#define GAB_VARIANT_H

typedef enum {
    VARIANT_NUMBER,
} VariantType;

typedef struct {
    VariantType type;
    union {
        float number;
    };
} Variant;

#endif
