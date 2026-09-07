#ifndef GAB_RUNTIME_H
#define GAB_RUNTIME_H

#include <stddef.h>

/* What a compiled program calls into. A native object embeds no host pointer, so each symbol here
 * takes only what the emitted code can name, and a program links against nothing else. */

void *gab_box(size_t size);

/* Frees one object; a null object frees nothing, which is what makes a moved-from slot safe to drop. */
void gab_free(void *object);

/* Ends the program where a check the compiler emitted has failed, such as an index out of range. */
void gab_trap(const char *message);

#endif
