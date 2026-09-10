#include "driver/compile.h"
#include "driver/interface.h"
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

/* What one compilation may be given. */
#define GABC_MAX_IMPORTS 64
#define GABC_MAX_FILES 16

/* Matched at the end, so a file is source because of what it is called and not what it contains. */
static bool has_extension(const char *path, const char *extension) {
    size_t length = strlen(path);
    size_t wanted = strlen(extension);

    return length > wanted && strcmp(path + length - wanted, extension) == 0;
}

static int usage(void) {
    fprintf(stderr, "usage: gabc [-c] [--core] [--out-dir <dir>] [-o <binary>]\n"
                    "            [--import <module>=<interface>]... <source.gab>...\n");
    return 2;
}

/* An import as the command line states it, which the compilation reads in the order it was given:
 * an interface names the modules it states, so those come before it. */
static bool parse_import(char *argument, const char **name, const char **path) {
    char *equals = strchr(argument, '=');

    if (!equals || equals == argument || !equals[1]) {
        return false;
    }

    *equals = '\0';

    *name = argument;
    *path = equals + 1;

    return true;
}

int main(int argc, char **argv) {
    const char *binary = NULL;
    const char *out_dir = ".";

    const char *paths[GABC_MAX_FILES];
    size_t path_count = 0;

    const char *extra[16];
    size_t extra_count = 0;

    const char *import_names[GABC_MAX_IMPORTS];
    const char *import_paths[GABC_MAX_IMPORTS];
    size_t import_count = 0;

    bool is_core = false;
    bool compile_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--core") == 0) {
            is_core = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            compile_only = true;
        } else if (strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            binary = argv[++i];
        } else if (strcmp(argv[i], "--import") == 0 && i + 1 < argc) {
            if (import_count == GABC_MAX_IMPORTS) {
                fprintf(stderr, "gabc: more than %d imports\n", GABC_MAX_IMPORTS);
                return 1;
            }

            if (!parse_import(argv[++i], &import_names[import_count], &import_paths[import_count])) {
                fprintf(stderr, "gabc: --import takes <module>=<interface>, not '%s'\n", argv[i]);
                return 1;
            }

            import_count++;
        } else if (argv[i][0] == '-') {
            return usage();
        } else if (has_extension(argv[i], ".gab")) {
            if (path_count == GABC_MAX_FILES) {
                fprintf(stderr, "gabc: more than %d source files\n", GABC_MAX_FILES);
                return 1;
            }

            paths[path_count++] = argv[i];
        } else if (extra_count < 16) {
            /* Anything not Gab source is handed to the link, which is how a program supplies its own
             * entry point or calls into C it already has. */
            extra[extra_count++] = argv[i];
        }
    }

    if (!path_count) {
        return usage();
    }

    const char *sources[GABC_MAX_FILES];

    for (size_t i = 0; i < path_count; i++) {
        sources[i] = read_file(paths[i]);

        if (!sources[i]) {
            fprintf(stderr, "gabc: %s: no such file\n", paths[i]);
            return 1;
        }
    }

    Arena *arena = arena_create(4096);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, arena, paths[0]);

    StringPool strings;
    string_pool_init(&strings, arena);

    /* Every artifact is named for the module the source declares, which is read before anything is
     * compiled so that what a compilation writes is known before it runs. */
    char module_name[64];

    bool ok = gab_module_name(sources[0], arena, &strings, module_name, sizeof(module_name), &diagnostics);

    char object[512];
    char interface[512];

    if (ok) {
        snprintf(object, sizeof(object), "%s/%s.o", out_dir, module_name);
        snprintf(interface, sizeof(interface), "%s/%s.gabi", out_dir, module_name);
    }

    GabDependency dependencies[GABC_MAX_IMPORTS + 1];
    size_t dependency_count = 0;

    char *texts[GABC_MAX_IMPORTS + 1] = {0};

    /* The core is found beside the compiler rather than named, so a program that imports nothing of
     * its own is compiled without a flag. The one compilation writing it reads none. */
    if (ok && !is_core) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.gabi", gab_libdir(), GAB_CORE_MODULE);

        texts[dependency_count] = gab_interface_read(path);

        if (!texts[dependency_count]) {
            fprintf(stderr, "gabc: the core is not installed: no %s\n", path);
            ok = false;
        } else {
            dependencies[dependency_count] = (GabDependency){
                .name = GAB_CORE_MODULE,
                .text = texts[dependency_count],
                .direct = true,
            };

            dependency_count++;
        }
    }

    for (size_t i = 0; ok && i < import_count; i++) {
        texts[dependency_count] = gab_interface_read(import_paths[i]);

        if (!texts[dependency_count]) {
            fprintf(stderr, "gabc: %s: no such interface\n", import_paths[i]);
            ok = false;
            break;
        }

        dependencies[dependency_count] = (GabDependency){
            .name = import_names[i],
            .text = texts[dependency_count],
            .direct = true,
        };

        dependency_count++;
    }

    GabCompiled compiled = {0};

    GabCompile request = {
        .sources = sources,
        .source_count = path_count,
        .names = paths,
        .object = object,
        .interface = compile_only ? interface : NULL,
        .dependencies = dependencies,
        .dependency_count = dependency_count,
        .writes_core = is_core,
    };

    ok = ok && gab_compile(&request, &compiled, &diagnostics);

    if (!ok) {
        diagnostics_print(&diagnostics, stderr);
    }

    if (ok && !compile_only) {
        /* The object beside each interface is what its declarations were compiled from, which the
         * link needs whether or not this module names it. */
        for (size_t i = 0; i < import_count && extra_count < 16; i++) {
            static char objects[GABC_MAX_IMPORTS][512];

            gab_object_beside(import_paths[i], objects[i], sizeof(objects[i]));

            extra[extra_count++] = objects[i];
        }

        const char *out = binary ? binary : module_name;

        ok = gab_link(object, compiled.module_name, extra, extra_count, out);

        if (!ok) {
            fprintf(stderr, "gabc: %s: the object did not link\n", out);
        }
    }

    for (size_t i = 0; i < dependency_count; i++) {
        free(texts[i]);
    }

    string_pool_free(&strings);
    diagnostics_free(&diagnostics);
    arena_destroy(arena);

    for (size_t i = 0; i < path_count; i++) {
        free((char *)sources[i]);
    }

    return ok ? 0 : 1;
}
