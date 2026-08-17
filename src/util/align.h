#ifndef GAB_ALIGN_H
#define GAB_ALIGN_H

#include <stddef.h>

// Rounds offset up to the next multiple of alignment, which must be a power of
// two. Used both by the arena and by struct layout.
static inline size_t align_up(size_t offset, size_t alignment) {
    return (offset + (alignment - 1)) & ~(alignment - 1);
}

#endif
