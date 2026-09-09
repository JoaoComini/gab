#include "driver/compile.h"
#include "driver/link.h"

#include "decl_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");

    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *text = malloc((size_t)size + 1);

    size_t read = fread(text, 1, (size_t)size, file);
    text[read] = '\0';

    fclose(file);

    return text;
}

static int usage(void) {
    fprintf(stderr, "usage: gabc [--core] [-c] [-L <dir>] -o <output> [<source.gab>...]\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *output = NULL;
    const char *paths[16];
    size_t path_count = 0;
    const char *extra[16];
    size_t extra_count = 0;
    const char *search[16];
    size_t search_count = 0;
    bool is_prelude = false;
    bool compile_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--core") == 0) {
            is_prelude = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            compile_only = true;
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc && search_count < 16) {
            search[search_count++] = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (argv[i][0] == '-') {
            return usage();
        } else if (strstr(argv[i], ".gab") && path_count < 16) {
            paths[path_count++] = argv[i];
        } else if (extra_count < 16) {
            /* Anything not Gab source is handed to the link, which is how a program supplies its own
             * entry point or calls into C it already has. */
            extra[extra_count++] = argv[i];
        }
    }

    if (!output || !path_count) {
        return usage();
    }

    /* The prelude is a library, so it is only ever compiled. */
    compile_only = compile_only || is_prelude;

    const char *sources[16];

    for (size_t i = 0; i < path_count; i++) {
        sources[i] = read_file(paths[i]);

        if (!sources[i]) {
            fprintf(stderr, "gabc: %s: no such file\n", paths[i]);
            return 1;
        }
    }

    char directory[512] = ".";

    if (path_count) {
        const char *slash = strrchr(paths[0], '/');

        if (slash) {
            snprintf(directory, sizeof(directory), "%.*s", (int)(slash - paths[0]), paths[0]);
        }
    }

    Arena *arena = arena_create(4096);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, arena, path_count ? paths[0] : GAB_CORE_MODULE);

    /* Linking reads an object, so one is written beside the binary even where it was not asked for. */
    char scratch[512];

    if (!compile_only) {
        snprintf(scratch, sizeof(scratch), "%s.o", output);
    }

    /* The prelude's declarations are written where a program later reads them, beside its object. */
    char interface[512] = {0};

    if (is_prelude) {
        snprintf(interface, sizeof(interface), "%s/%s.gabi", gab_libdir(), GAB_CORE_MODULE);
    } else if (compile_only) {
        /* A module compiled to an object states what importers may name, beside the object itself. */
        snprintf(interface, sizeof(interface), "%.*s.gabi", (int)strlen(output) - 2, output);
    }

    GabCompile request = {
        .module = path_count ? paths[0] : NULL,
        .sources = sources,
        .source_count = path_count,
        .names = paths,
        .object = compile_only ? output : scratch,
        .interface = interface[0] ? interface : NULL,
        .allow_primitive_impls = is_prelude,
        .search = search,
        .search_count = search_count,
        .source_directory = directory,
    };

    bool ok = gab_compile(&request, &diagnostics);

    if (!ok) {
        diagnostics_print(&diagnostics, stderr);
    }

    if (ok && !compile_only) {
        for (size_t i = 0; i < request.resolved_count && extra_count < 16; i++) {
            extra[extra_count++] = request.resolved[i];
        }

        ok = gab_link(request.object, request.module_name, extra, extra_count, output);

        if (!ok) {
            fprintf(stderr, "gabc: %s: the object did not link\n", output);
        }
    }

    diagnostics_free(&diagnostics);
    arena_destroy(arena);

    for (size_t i = 0; i < path_count; i++) {
        free((char *)sources[i]);
    }

    return ok ? 0 : 1;
}
