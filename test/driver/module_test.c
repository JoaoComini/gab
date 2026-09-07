#include "driver/compile.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GAB_TEST_SCRATCH
#define GAB_TEST_SCRATCH "."
#endif

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");

    assert(file);

    fputs(text, file);
    fclose(file);
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");

    if (!file) {
        return NULL;
    }

    static char text[1 << 14];
    size_t read = fread(text, 1, sizeof(text) - 1, file);

    text[read] = '\0';
    fclose(file);

    return text;
}

static bool compile(const char *source, const char *object, const char *interface, const char *import) {
    char path[512];
    snprintf(path, sizeof(path), "%s/module_test_unit.gab", GAB_TEST_SCRATCH);

    write_file(path, source);

    char *text = read_file(path);

    Arena *arena = arena_create(4096);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, arena, path);

    const char *imports[1];
    size_t import_count = 0;

    if (import) {
        imports[import_count++] = import;
    }

    GabCompile request = {
        .source = text,
        .object = object,
        .interface = interface,
        .imports = imports,
        .import_count = import_count,
    };

    bool ok = gab_compile(&request, &diagnostics);

    diagnostics_free(&diagnostics);
    arena_destroy(arena);

    return ok;
}

/* What one unit states, another names: the declarations cross as an interface, never as source. */
static void a_unit_names_what_an_imported_interface_declares(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/module_test_lib.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/module_test_lib.gabi", GAB_TEST_SCRATCH);

    assert(compile("module lib;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    char import[1024];
    snprintf(import, sizeof(import), "lib=%s", interface);

    char user[512];
    snprintf(user, sizeof(user), "%s/module_test_use.o", GAB_TEST_SCRATCH);

    assert(compile("module use;\nimport lib;\nfunc main(): i32 { return lib::helper(); }\n", user, NULL,
                   import));
}

static void a_module_is_named_only_where_it_is_imported(void) {
    char interface[512];
    snprintf(interface, sizeof(interface), "%s/module_test_lib.gabi", GAB_TEST_SCRATCH);

    char import[1024];
    snprintf(import, sizeof(import), "lib=%s", interface);

    char object[512];
    snprintf(object, sizeof(object), "%s/module_test_undeclared.o", GAB_TEST_SCRATCH);

    assert(!compile("module use;\nfunc main(): i32 { return lib::helper(); }\n", object, NULL, import));
}

int main(void) {
    a_unit_names_what_an_imported_interface_declares();
    a_module_is_named_only_where_it_is_imported();

    printf("module tests passed\n");

    return 0;
}
