#ifndef GAB_ALIGN_H
#define GAB_ALIGN_H

#include <stddef.h>

static inline size_t align_up(size_t offset, size_t alignment) {
    return (offset + (alignment - 1)) & ~(alignment - 1);
}

#endif
