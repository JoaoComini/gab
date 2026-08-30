#ifndef GAB_TYPE_LAYOUT_H
#define GAB_TYPE_LAYOUT_H

#include "type.h"

#include <stddef.h>
#include <stdint.h>

typedef struct DropPlan DropPlan;

typedef enum {
    DROP_BOX,

    DROP_BLOCK,

    DROP_ARRAY,

    DROP_FIELDS,
} DropKind;

typedef struct DropStep {
    size_t offset;
    const DropPlan *plan;
} DropStep;

struct DropPlan {
    DropKind kind;

    const DropPlan *inner;

    size_t stride;

    int32_t length;

    const DropStep *steps;
    size_t step_count;
};

typedef struct TypeLayout {
    size_t size;
    size_t alignment;

    const size_t *offsets;
    size_t offset_count;
} TypeLayout;

#endif
