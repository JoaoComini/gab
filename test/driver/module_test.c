#include "driver/compile.h"
#include "driver/interface.h"
#include "driver/link.h"

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

static bool compile(const char *source, const char *object, const char *interface, const char *search) {
    char path[512];
    snprintf(path, sizeof(path), "%s/module_test_unit.gab", GAB_TEST_SCRATCH);

    write_file(path, source);

    char *text = read_file(path);

    Arena *arena = arena_create(4096);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, arena, path);

    const char *directories[1];
    size_t directory_count = 0;

    if (search) {
        directories[directory_count++] = search;
    }

    GabCompile request = {
        .source = text,
        .object = object,
        .interface = interface,
        .search = directories,
        .search_count = directory_count,
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

    snprintf(object, sizeof(object), "%s/lib.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/lib.gabi", GAB_TEST_SCRATCH);

    assert(compile("module lib;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/module_test_use.o", GAB_TEST_SCRATCH);

    assert(compile("module use;\nimport lib;\nfunc main(): i32 { return lib::helper(); }\n", user, NULL,
                   GAB_TEST_SCRATCH));
}

static void a_module_is_named_only_where_it_is_imported(void) {
    char object[512];
    snprintf(object, sizeof(object), "%s/module_test_undeclared.o", GAB_TEST_SCRATCH);

    assert(!compile("module use;\nfunc main(): i32 { return lib::helper(); }\n", object, NULL,
                    GAB_TEST_SCRATCH));
}

/* An interface and the object it was compiled from name each other, so a stale pair cannot be linked. */
static void a_stale_interface_does_not_link(void) {
    char object[512];
    char interface[512];
    char stale[512];

    snprintf(object, sizeof(object), "%s/dig.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/dig.gabi", GAB_TEST_SCRATCH);
    snprintf(stale, sizeof(stale), "%s/stale/dig.gabi", GAB_TEST_SCRATCH);

    assert(compile("module dig;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    /* The interface kept while the object moves on, which is the pairing a digest is there to catch. */
    char make[1024];
    snprintf(make, sizeof(make), "mkdir -p %s/stale", GAB_TEST_SCRATCH);
    assert(system(make) == 0);

    char *kept = gab_interface_read(interface);
    assert(kept);

    write_file(stale, kept);
    free(kept);

    assert(compile("module dig;\nfunc helper(a: i32): i32 { return a; }\n", object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/module_test_stale_user.o", GAB_TEST_SCRATCH);

    char stale_directory[512];
    snprintf(stale_directory, sizeof(stale_directory), "%s/stale", GAB_TEST_SCRATCH);

    /* It compiles: the interface is well formed, and only the link can see it is not the object's. */
    assert(compile("module u;\nimport dig;\nfunc main(): i32 { return dig::helper(); }\n", user, NULL,
                   stale_directory));

    char binary[512];
    snprintf(binary, sizeof(binary), "%s/module_test_stale_user", GAB_TEST_SCRATCH);

    assert(!gab_link(user, "u", (const char *const[]){object}, 1, binary));
}

int main(void) {
    a_unit_names_what_an_imported_interface_declares();
    a_module_is_named_only_where_it_is_imported();
    a_stale_interface_does_not_link();

    printf("module tests passed\n");

    return 0;
}
