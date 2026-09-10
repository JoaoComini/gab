#ifndef GAB_DRIVER_LINK_H
#define GAB_DRIVER_LINK_H

#include <stdbool.h>
#include <stddef.h>

const char *gab_libdir(void);

void gab_object_beside(const char *interface, char *out, size_t capacity);

bool gab_link(const char *object, const char *module, const char *const *extra, size_t extra_count,
              const char *binary);

#endif
