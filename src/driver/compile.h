#ifndef GAB_DRIVER_COMPILE_H
#define GAB_DRIVER_COMPILE_H

#include "diagnostics.h"

#include <stdbool.h>

/* One compilation: source text in, a native object out. The core is compiled by the same call that
 * compiles a program, distinguished only by whether it may declare methods on the primitives. */
typedef struct {
    const char *module;

    const char *source;

    const char *object;

    /* Where the declarations a later compilation reads are written, when compiling the core. */
    const char *interface;

    /* The core declares 'impl str' and 'impl<T> slice<T>', which a program may not. */
    bool is_core;

    /* The module the source named, which the entry point a link writes must call into. */
    char module_name[64];

    /* Each '<module>=<path.gabi>' this compilation may import, whose declarations it resolves against. */
    const char *const *imports;
    size_t import_count;
} GabCompile;

/* False where the source does not compile, having reported why to 'diagnostics'. */
bool gab_compile(GabCompile *request, Diagnostics *diagnostics);

#endif
