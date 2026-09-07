#ifndef GAB_DRIVER_COMPILE_H
#define GAB_DRIVER_COMPILE_H

#include "diagnostics.h"

#include <stddef.h>

#include <stdbool.h>

/* One compilation: source text in, a native object out. The core is compiled by the same call that
 * compiles a program, distinguished only by whether it may declare methods on the primitives. */
typedef struct {
    const char *module;

    /* The files this module is written across, resolved together as one unit. */
    const char *const *sources;
    size_t source_count;

    /* What each source is called, so a diagnostic about one names the file it came from. */
    const char *const *names;

    const char *object;

    /* Where the declarations a later compilation reads are written, when compiling the core. */
    const char *interface;

    /* The core declares 'impl str' and 'impl<T> slice<T>', which a program may not. */
    bool is_core;

    /* The module the source named, which the entry point a link writes must call into. */
    char module_name[64];

    /* The directory holding the source, which an interface is looked for in before the compiler's own. */
    const char *source_directory;

    /* Where an interface the source imports is looked for, before the source's own directory. */
    const char *const *search;
    size_t search_count;

    /* Filled with the object beside each interface an import resolved to, which the link then needs. */
    char resolved[8][512];
    size_t resolved_count;
} GabCompile;

/* False where the source does not compile, having reported why to 'diagnostics'. */
bool gab_compile(GabCompile *request, Diagnostics *diagnostics);

#endif
