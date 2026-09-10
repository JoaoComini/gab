#ifndef GAB_DRIVER_COMPILE_H
#define GAB_DRIVER_COMPILE_H

#include "diagnostics.h"
#include "memory/arena.h"
#include "string/string_pool.h"

#include <stddef.h>

#include <stdbool.h>

typedef struct {
    const char *name;
    const char *text;

    bool direct;
} GabDependency;

typedef struct {
    char module_name[64];
} GabCompiled;

typedef struct {
    const char *const *sources;
    size_t source_count;

    const char *const *names;

    const char *object;

    const char *interface;

    const GabDependency *dependencies;
    size_t dependency_count;

    bool writes_core;
} GabCompile;

bool gab_module_name(const char *source, Arena *arena, StringPool *strings, char *out, size_t capacity,
                     Diagnostics *diagnostics);

bool gab_compile(const GabCompile *request, GabCompiled *out, Diagnostics *diagnostics);

#endif
