#ifndef RULE_VARIANT_H
#define RULE_VARIANT_H

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
