#ifndef GAB_TYPE_LAYOUT_H
#define GAB_TYPE_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct TypeLayout {
    size_t size;
    size_t alignment;

    const size_t *offsets;
    size_t offset_count;
} TypeLayout;

#endif
