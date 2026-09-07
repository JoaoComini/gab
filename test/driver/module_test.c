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

static bool compile_all(const char *const *sources, size_t count, const char *object, const char *interface,
                        const char *search) {
    Arena *arena = arena_create(4096);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, arena, "a test");

    const char *directories[1];
    size_t directory_count = 0;

    if (search) {
        directories[directory_count++] = search;
    }

    GabCompile request = {
        .sources = sources,
        .source_count = count,
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

static bool compile(const char *source, const char *object, const char *interface, const char *search) {
    const char *one[1] = {source};

    return compile_all(one, 1, object, interface, search);
}

/* One module written across two files, which are resolved together rather than one at a time. */
static void a_module_is_written_across_the_files_it_is_compiled_from(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/split.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/split.gabi", GAB_TEST_SCRATCH);

    const char *parts[2] = {"module split;\nfunc one(): i32 { return 1; }\n",
                            "module split;\nfunc two(): i32 { return one() + 1; }\n"};

    assert(compile_all(parts, 2, object, interface, NULL));

    char *stated = gab_interface_read(interface);

    assert(stated);
    assert(strstr(stated, "func one(): i32;"));
    assert(strstr(stated, "func two(): i32;"));

    free(stated);
}

static void every_file_of_a_module_declares_that_module(void) {
    char object[512];
    snprintf(object, sizeof(object), "%s/disagree.o", GAB_TEST_SCRATCH);

    const char *parts[2] = {"module split;\nfunc one(): i32 { return 1; }\n",
                            "module other;\nfunc two(): i32 { return 2; }\n"};

    assert(!compile_all(parts, 2, object, NULL, NULL));
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

/* A module reached only through another is linked too, which the interface's imports are what state. */
static void an_import_of_an_import_is_linked(void) {
    char deep[512];
    char deep_interface[512];

    snprintf(deep, sizeof(deep), "%s/deep.o", GAB_TEST_SCRATCH);
    snprintf(deep_interface, sizeof(deep_interface), "%s/deep.gabi", GAB_TEST_SCRATCH);

    assert(compile("module deep;\nfunc bottom(): i32 { return 4; }\n", deep, deep_interface, NULL));

    char middle[512];
    char middle_interface[512];

    snprintf(middle, sizeof(middle), "%s/middle.o", GAB_TEST_SCRATCH);
    snprintf(middle_interface, sizeof(middle_interface), "%s/middle.gabi", GAB_TEST_SCRATCH);

    assert(compile("module middle;\nimport deep;\nfunc up(): i32 { return deep::bottom(); }\n", middle,
                   middle_interface, GAB_TEST_SCRATCH));

    char *stated = gab_interface_read(middle_interface);

    assert(stated);
    assert(strstr(stated, "import deep;"));

    free(stated);

    char top[512];
    snprintf(top, sizeof(top), "%s/top.o", GAB_TEST_SCRATCH);

    GabCompile request = {
        .sources =
            (const char *const[]){"module top;\nimport middle;\nfunc main(): i32 { return middle::up(); }\n"},
        .source_count = 1,
        .object = top,
        .search = (const char *const[]){GAB_TEST_SCRATCH},
        .search_count = 1};

    Arena *arena = arena_create(4096);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, arena, "a test");

    assert(gab_compile(&request, &diagnostics));

    /* 'deep' is named by nothing this unit wrote, and compiling it still reached the object. */
    bool reached = false;

    for (size_t i = 0; i < request.resolved_count; i++) {
        reached = reached || strstr(request.resolved[i], "deep.o") != NULL;
    }

    assert(reached);

    diagnostics_free(&diagnostics);
    arena_destroy(arena);
}

int main(void) {
    a_unit_names_what_an_imported_interface_declares();
    a_module_is_named_only_where_it_is_imported();
    a_stale_interface_does_not_link();
    a_module_is_written_across_the_files_it_is_compiled_from();
    every_file_of_a_module_declares_that_module();
    an_import_of_an_import_is_linked();

    printf("module tests passed\n");

    return 0;
}
