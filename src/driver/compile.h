#ifndef GAB_DRIVER_COMPILE_H
#define GAB_DRIVER_COMPILE_H

#include "diagnostics.h"
#include "memory/arena.h"
#include "string/string_pool.h"

#include <stddef.h>

#include <stdbool.h>

/* One module this compilation reads rather than compiles: what it declares, as the interface text a
 * caller found and read. A direct import is one the source may name; the rest are read so that what
 * they declare can be resolved against, and linked. */
typedef struct {
    const char *name;
    const char *text;

    bool direct;
} GabDependency;

/* What a compilation concluded that its caller cannot know: the module the source named, which an
 * entry point must call into. */
typedef struct {
    char module_name[64];
} GabCompiled;

/* One compilation: source text in, a native object out. Every file it reads is named here; the
 * compilation itself opens nothing but the object it writes, so what a module depends on is settled
 * before it starts rather than discovered while it runs. */
typedef struct {
    /* The files this module is written across, resolved together as one unit. */
    const char *const *sources;
    size_t source_count;

    /* What each source is called, so a diagnostic about one names the file it came from. */
    const char *const *names;

    const char *object;

    /* Where the declarations a later compilation reads are written. */
    const char *interface;

    /* What this module imports, in the order it reads them: an interface names the modules it states,
     * so those come before it. The core is one of these, which the caller finds rather than names. */
    const GabDependency *dependencies;
    size_t dependency_count;

    /* True in the one compilation that writes the core, which reads none and may declare intrinsics. */
    bool writes_core;
} GabCompile;

/* The module 'source' declares, written into 'out': what every artifact of a compilation is named for,
 * which its caller reads before compiling so that what it writes is known before it runs. */
bool gab_module_name(const char *source, Arena *arena, StringPool *strings, char *out, size_t capacity,
                     Diagnostics *diagnostics);

/* False where the source does not compile, having reported why to 'diagnostics'. What the compilation
 * concluded is written to 'out'. */
bool gab_compile(const GabCompile *request, GabCompiled *out, Diagnostics *diagnostics);

#endif
