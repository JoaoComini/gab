#include "ast/resolve.h"
#include "driver/compile.h"
#include "driver/interface.h"
#include "driver/link.h"

#include "support/run.h"

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

/* Nothing was compiled for arguments the declaring unit never saw, so a reader instantiates the body
 * its interface carries rather than linking against a symbol that does not exist. */
static void an_imported_generic_is_instantiated_where_it_is_named(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/gen.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/gen.gabi", GAB_TEST_SCRATCH);

    assert(compile("module gen;\nfunc same<T>(x: T): T { return x; }\n", object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/module_test_generic.o", GAB_TEST_SCRATCH);

    assert(compile("module use;\nimport gen;\nfunc main(): i32 { return gen::same(7); }\n", user, NULL,
                   GAB_TEST_SCRATCH));

    char binary[512];
    snprintf(binary, sizeof(binary), "%s/module_test_generic", GAB_TEST_SCRATCH);

    /* The instance is a body of the reader's own, so linking finds it without the declaring object. */
    assert(gab_link(user, "use", (const char *const[]){object}, 1, binary));
}

/* Two readers naming one generic each instantiate it, so the two objects state the same body and the
 * link takes one rather than refusing both. */
static void one_generic_instantiated_twice_links_once(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/twice.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/twice.gabi", GAB_TEST_SCRATCH);

    assert(compile("module twice;\nfunc same<T>(x: T): T { return x; }\n", object, interface, NULL));

    char first[512];
    char first_interface[512];

    snprintf(first, sizeof(first), "%s/twice_one.o", GAB_TEST_SCRATCH);
    snprintf(first_interface, sizeof(first_interface), "%s/one.gabi", GAB_TEST_SCRATCH);

    assert(compile("module one;\nimport twice;\nfunc up(): i32 { return twice::same(1); }\n", first,
                   first_interface, GAB_TEST_SCRATCH));

    char user[512];
    snprintf(user, sizeof(user), "%s/twice_use.o", GAB_TEST_SCRATCH);

    assert(compile("module use;\nimport twice;\nimport one;\n"
                   "func main(): i32 { return twice::same(2) + one::up(); }\n",
                   user, NULL, GAB_TEST_SCRATCH));

    char binary[512];
    snprintf(binary, sizeof(binary), "%s/twice_linked", GAB_TEST_SCRATCH);

    assert(gab_link(user, "use", (const char *const[]){object, first}, 2, binary));
}

/* A method an imported generic type owns is instantiated where it is named, as a free function is. */
static void an_imported_generic_method_is_instantiated_where_it_is_named(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/holder.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/holder.gabi", GAB_TEST_SCRATCH);

    assert(compile("module holder;\nstruct Box<T> { value: T }\n"
                   "impl<T> Box<T> {\n    func get(self: &Self): T { return self.value; }\n}\n",
                   object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/holder_use.o", GAB_TEST_SCRATCH);

    assert(compile("module use;\nimport holder;\n"
                   "func main(): i32 { let b: holder::Box<i32> = holder::Box<i32> { value: 3 };\n"
                   "                   return b.get(); }\n",
                   user, NULL, GAB_TEST_SCRATCH));

    char binary[512];
    snprintf(binary, sizeof(binary), "%s/holder_linked", GAB_TEST_SCRATCH);

    assert(gab_link(user, "use", (const char *const[]){object}, 1, binary));
}

typedef struct {
    TestContext ctx;
    Scope *scope;
    ResolvedUnit *resolved;
    ASTUnit *unit;
} Reading;

/* Both readings share one string pool, as the writer and the reader of an interface do in one
 * compilation, since an id compares by the pointers interning produced. */
static void read_source(Reading *reading, TestContext *ctx, const char *source) {
    reading->scope = scope_create(ctx->arena, &ctx->strings, NULL);
    reading->unit = ast_unit_create(ctx->arena);

    bool ok =
        test_resolve_ir_with(ctx, reading->scope, &reading->unit, NULL, &reading->resolved, source, false);

    assert(ok);
}

static DeclId method_id_in(Reading *reading, TestContext *ctx, const char *type, const char *method) {
    const Type *owner = scope_type_lookup(reading->scope, string_from_cstr(&ctx->strings, type));

    assert(owner);

    Function *found = function_registry_find_owned(reading->resolved->functions, owner,
                                                   string_from_cstr(&ctx->strings, method));

    assert(found);
    assert(decl_id_is_set(found->decl->id));

    return found->decl->id;
}

static DeclId type_id_in(Reading *reading, TestContext *ctx, const char *name) {
    const Type *type = scope_type_lookup(reading->scope, string_from_cstr(&ctx->strings, name));

    assert(type);
    assert(decl_id_is_set(type_decl(type)->id));

    return type_decl(type)->id;
}

/* What a writer declares and what a reader of its interface declares are the same declaration. */
static void a_declaration_read_back_has_the_id_it_was_written_with(void) {
    const char *source = "module holder;\nstruct Box { value: i32 }\n"
                         "impl Box {\n    func get(self: &Self): i32 { return self.value; }\n}\n";

    TestContext ctx;
    test_context_init(&ctx);

    Reading writer;
    read_source(&writer, &ctx, source);

    char path[512];
    snprintf(path, sizeof(path), "%s/id_written.gabi", GAB_TEST_SCRATCH);

    assert(gab_interface_write(writer.unit, &writer.resolved->facts, path));

    char *text = gab_interface_read(path);
    assert(text);

    Reading reader;
    read_source(&reader, &ctx, text);

    DeclId written = method_id_in(&writer, &ctx, "Box", "get");

    assert(written.module && written.owner);
    assert(decl_id_equals(written, method_id_in(&reader, &ctx, "Box", "get")));

    assert(decl_id_equals(type_id_in(&writer, &ctx, "Box"), type_id_in(&reader, &ctx, "Box")));

    free(text);
    test_context_free(&ctx);
}

static void two_modules_declare_one_foreign_function(void) {
    char first[512];
    char first_interface[512];

    snprintf(first, sizeof(first), "%s/ffi_one.o", GAB_TEST_SCRATCH);
    snprintf(first_interface, sizeof(first_interface), "%s/ffi_one.gabi", GAB_TEST_SCRATCH);

    assert(compile("module ffi_one;\nextern \"C\" func getpid(): i32;\n"
                   "func stop(): i32 { return getpid(); }\n",
                   first, first_interface, NULL));

    char second[512];
    char second_interface[512];

    snprintf(second, sizeof(second), "%s/ffi_two.o", GAB_TEST_SCRATCH);
    snprintf(second_interface, sizeof(second_interface), "%s/ffi_two.gabi", GAB_TEST_SCRATCH);

    assert(compile("module ffi_two;\nextern \"C\" func getpid(): i32;\n"
                   "func halt(): i32 { return getpid(); }\n",
                   second, second_interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/ffi_use.o", GAB_TEST_SCRATCH);

    assert(compile("module use;\nimport ffi_one;\nimport ffi_two;\n"
                   "func main(): i32 { return ffi_one::stop() - ffi_two::halt(); }\n",
                   user, NULL, GAB_TEST_SCRATCH));

    char binary[512];
    snprintf(binary, sizeof(binary), "%s/ffi_linked", GAB_TEST_SCRATCH);

    assert(gab_link(user, "use", (const char *const[]){first, second}, 2, binary));
}

int main(void) {
    two_modules_declare_one_foreign_function();
    a_declaration_read_back_has_the_id_it_was_written_with();
    an_imported_generic_method_is_instantiated_where_it_is_named();
    one_generic_instantiated_twice_links_once();
    an_imported_generic_is_instantiated_where_it_is_named();
    a_unit_names_what_an_imported_interface_declares();
    a_module_is_named_only_where_it_is_imported();
    a_stale_interface_does_not_link();
    a_module_is_written_across_the_files_it_is_compiled_from();
    every_file_of_a_module_declares_that_module();
    an_import_of_an_import_is_linked();

    printf("module tests passed\n");

    return 0;
}
