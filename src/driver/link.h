#ifndef GAB_DRIVER_LINK_H
#define GAB_DRIVER_LINK_H

#include <stdbool.h>
#include <stddef.h>

/* Where the core and the runtime a program links against are found: beside the compiler, so moving an
 * installation moves what it links. 'GABC_LIBDIR' overrides it. */
const char *gab_libdir(void);

/* Links one object into an executable, together with any files named in 'extra'. Those supply the entry
 * point where they define one, and otherwise it is written to call 'module's 'main'. The C compiler
 * drives the link because it, and not a linker, knows where this system keeps libc and its startup
 * objects. */
bool gab_link(const char *object, const char *module, const char *const *extra, size_t extra_count,
              const char *binary);

#endif
