#ifndef GAB_DRIVER_COMPILE_H
#define GAB_DRIVER_COMPILE_H

#include "diagnostics.h"

#include <stddef.h>

#include <stdbool.h>

/* The objects a link needs beside the one compiled: one for each interface an import resolved to. */
typedef struct {
    char (*objects)[512];
    size_t count;
    size_t capacity;
} GabResolvedObjects;

/* What a compilation concluded that its caller cannot know: the module the source named, which an
 * entry point must call into, and the objects its imports resolved to. */
typedef struct {
    char module_name[64];

    GabResolvedObjects resolved;
} GabCompiled;

/* One compilation: source text in, a native object out. The prelude is compiled by the same call that
 * compiles a program, distinguished only by whether it may declare methods on the primitives. */
typedef struct {
    const char *module;

    /* The files this module is written across, resolved together as one unit. */
    const char *const *sources;
    size_t source_count;

    /* What each source is called, so a diagnostic about one names the file it came from. */
    const char *const *names;

    const char *object;

    /* Where the declarations a later compilation reads are written. */
    const char *interface;

    /* The prelude declares 'impl str' and 'impl<T> slice<T>', which a program may not; it is also the
     * one compilation that does not read the prelude, being what writes it. */
    bool declares_intrinsics;

    /* The directory holding the source, which an interface is looked for in before the compiler's own. */
    const char *source_directory;

    /* Where an interface the source imports is looked for, before the source's own directory. */
    const char *const *search;
    size_t search_count;
} GabCompile;

/* False where the source does not compile, having reported why to 'diagnostics'. What the compilation
 * concluded is written to 'out', whose 'resolved' the caller sizes: an import past that capacity is an
 * error rather than an object the link silently goes without. */
bool gab_compile(const GabCompile *request, GabCompiled *out, Diagnostics *diagnostics);

#endif
