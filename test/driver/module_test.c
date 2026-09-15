#include "ast/resolve.h"
#include "driver/compile.h"
#include "driver/interface.h"
#include "driver/link.h"
#include "syntax/parser.h"

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

/* Every path a test names lives beside the others in the scratch directory, so a name is all a
 * case states and where it lives is this one line's to know. */
static const char *scratch(const char *name) {
    static char paths[16][512];
    static size_t next = 0;

    char *path = paths[next++ % 16];

    snprintf(path, 512, "%s/%s", GAB_TEST_SCRATCH, name);

    return path;
}

/* What a compilation is given: every interface it reads, named. A test states a directory rather than
 * each import, so the module a source names is read from '<search>/<module>.gabi' as a build would
 * have passed it. */
static bool compile_all(const char *const *sources, size_t count, const char *object, const char *interface,
                        const char *search) {
    Arena *arena = arena_create(4096);

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, arena, "a test");

    StringPool strings;
    string_pool_init(&strings, arena);

    GabDependency dependencies[16];
    size_t dependency_count = 0;

    char *texts[16] = {0};

    bool ok = true;

    /* The core, which every compilation but its own reads. */
    char core[512];
    snprintf(core, sizeof(core), "%s/%s.gabi", gab_libdir(), GAB_CORE_MODULE);

    texts[dependency_count] = gab_interface_read(core);

    if (!texts[dependency_count]) {
        ok = false;
    } else {
        dependencies[dependency_count] =
            (GabDependency){.name = GAB_CORE_MODULE, .text = texts[dependency_count], .direct = true};
        dependency_count++;
    }

    /* Each import every source states, read in the order stated: an interface names what it states,
     * so a test lists a module after whatever it imports. */
    for (size_t i = 0; ok && search && i < count; i++) {
        ASTFile *file = NULL;

        if (!parse_header(sources[i], arena, &strings, &file, &diagnostics)) {
            continue;
        }

        for (size_t j = 0; j < file->imports.size; j++) {
            const char *name = file->imports.data[j].name->name->data;

            if (strcmp(name, GAB_CORE_MODULE) == 0 || dependency_count == 16) {
                continue;
            }

            char path[512];
            snprintf(path, sizeof(path), "%s/%s.gabi", search, name);

            texts[dependency_count] = gab_interface_read(path);

            if (!texts[dependency_count]) {
                continue;
            }

            static char names[16][128];
            snprintf(names[dependency_count], sizeof(names[0]), "%s", name);

            dependencies[dependency_count] = (GabDependency){
                .name = names[dependency_count], .text = texts[dependency_count], .direct = true};
            dependency_count++;
        }
    }

    GabCompile request = {
        .sources = sources,
        .source_count = count,
        .object = object,
        .interface = interface,
        .dependencies = dependencies,
        .dependency_count = dependency_count,
    };

    GabCompiled compiled = {0};

    ok = ok && gab_compile(&request, &compiled, &diagnostics);

    for (size_t i = 0; i < dependency_count; i++) {
        free(texts[i]);
    }

    string_pool_free(&strings);
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
    const char *object = scratch("split.o");
    const char *interface = scratch("split.gabi");

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
    const char *object = scratch("disagree.o");

    const char *parts[2] = {"module split;\nfunc one(): i32 { return 1; }\n",
                            "module other;\nfunc two(): i32 { return 2; }\n"};

    assert(!compile_all(parts, 2, object, NULL, NULL));
}

/* What one unit states, another names: the declarations cross as an interface, never as source. */
static void a_unit_names_what_an_imported_interface_declares(void) {
    const char *object = scratch("lib.o");
    const char *interface = scratch("lib.gabi");

    assert(compile("module lib;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    const char *user = scratch("module_test_use.o");

    assert(compile("module use;\nimport lib;\nfunc main(): i32 { return lib::helper(); }\n", user, NULL,
                   GAB_TEST_SCRATCH));
}

static void a_module_is_named_only_where_it_is_imported(void) {
    const char *object = scratch("module_test_undeclared.o");

    assert(!compile("module use;\nfunc main(): i32 { return lib::helper(); }\n", object, NULL,
                    GAB_TEST_SCRATCH));
}

static void an_import_is_named_only_in_the_file_that_imports_it(void) {
    const char *object = scratch("perfile.o");
    const char *interface = scratch("perfile.gabi");

    assert(compile("module perfile;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    const char *user = scratch("perfile_use.o");

    const char *parts[2] = {"module use;\nimport perfile;\nfunc one(): i32 { return perfile::helper(); }\n",
                            "module use;\nfunc two(): i32 { return perfile::helper(); }\n"};

    assert(!compile_all(parts, 2, user, NULL, GAB_TEST_SCRATCH));
}

/* A field's type is resolved after every file has declared, so it reads the imports of its own file. */
static void a_field_names_a_type_its_own_file_imports(void) {
    const char *object = scratch("fieldlib.o");
    const char *interface = scratch("fieldlib.gabi");

    assert(compile("module fieldlib;\nstruct Held { value: i32, }\n", object, interface, NULL));

    const char *user = scratch("fieldlib_use.o");

    const char *parts[2] = {"module use;\nimport fieldlib;\nstruct Wrap { held: fieldlib::Held, }\n",
                            "module use;\nfunc main(): i32 { return 0; }\n"};

    assert(compile_all(parts, 2, user, NULL, GAB_TEST_SCRATCH));
}

/* An interface states the module, so a module names an import once however many of its files import it. */
static void an_interface_states_an_import_once(void) {
    const char *object = scratch("shared.o");
    const char *interface = scratch("shared.gabi");

    assert(compile("module shared;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    const char *user = scratch("twice.o");
    const char *stated = scratch("twice.gabi");

    const char *parts[2] = {"module twice;\nimport shared;\nfunc one(): i32 { return shared::helper(); }\n",
                            "module twice;\nimport shared;\nfunc two(): i32 { return shared::helper(); }\n"};

    assert(compile_all(parts, 2, user, stated, GAB_TEST_SCRATCH));

    char *written = gab_interface_read(stated);

    assert(written);

    const char *first = strstr(written, "import shared;");

    assert(first);
    assert(!strstr(first + 1, "import shared;"));

    free(written);
}

/* What a file declares is the module's, so a sibling names it without an import. */
static void a_declaration_is_named_across_the_files_of_its_module(void) {
    const char *object = scratch("across.o");
    const char *interface = scratch("across.gabi");

    const char *parts[2] = {"module across;\nfunc one(): i32 { return 1; }\n",
                            "module across;\nfunc two(): i32 { return one() + 1; }\n"};

    assert(compile_all(parts, 2, object, interface, NULL));
}

/* A module and its files are one namespace, so a name one file declares collides with the other's. */
static void a_name_two_files_declare_is_declared_twice(void) {
    const char *object = scratch("collide.o");

    const char *parts[2] = {"module collide;\nfunc same(): i32 { return 1; }\n",
                            "module collide;\nfunc same(): i32 { return 2; }\n"};

    assert(!compile_all(parts, 2, object, NULL, NULL));
}

/* The prelude declares into the global scope, which a module's own declaration shadows. */
static void a_module_declares_a_name_the_prelude_holds(void) {
    const char *object = scratch("shadows.o");

    assert(compile("module shadows;\nstruct Location { x: i32, }\n"
                   "func main(): i32 { let l = Location { x: 5 }; return l.x; }\n",
                   object, NULL, NULL));
}

/* An import binds the module in the file that wrote it, so a declaration cannot take the same name. */
static void a_declaration_does_not_take_the_name_of_an_import(void) {
    const char *object = scratch("taken.o");
    const char *interface = scratch("taken.gabi");

    assert(compile("module taken;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    const char *user = scratch("taken_use.o");

    assert(!compile("module use;\nimport taken;\nfunc taken(): i32 { return 1; }\n", user, NULL,
                    GAB_TEST_SCRATCH));
}

/* A module imported directly and reached through another as well is still one this module may name. */
static void an_import_is_read_before_whoever_imports_it(void) {
    const char *deep_object = scratch("deep.o");
    const char *deep_interface = scratch("deep.gabi");

    assert(compile("module deep;\nstruct Cell { value: i32 }\n", deep_object, deep_interface, NULL));

    const char *middle_object = scratch("middle.o");
    const char *middle_interface = scratch("middle.gabi");

    assert(compile("module middle;\nimport deep;\nfunc hold(): deep::Cell { return deep::Cell{value: 7}; }\n",
                   middle_object, middle_interface, GAB_TEST_SCRATCH));

    const char *user = scratch("deep_use.o");

    assert(compile("module use;\nimport middle;\n"
                   "func main(): i32 { return middle::hold().value; }\n",
                   user, NULL, GAB_TEST_SCRATCH));
}

static void a_module_that_imports_itself_is_an_error(void) {
    const char *object = scratch("loop.o");
    const char *interface = scratch("loop.gabi");

    assert(compile("module loop;\nfunc value(): i32 { return 1; }\n", object, interface, NULL));

    /* Restated so the interface imports itself, which no compilation of the source could write. */
    write_file(interface, "module loop;\nimport loop;\nfunc value(): i32;\n");

    const char *user = scratch("loop_use.o");

    assert(!compile("module use;\nimport loop;\nfunc main(): i32 { return loop::value(); }\n", user, NULL,
                    GAB_TEST_SCRATCH));
}

/* A name is qualified by the module that declares it, so one module's name does not reach another's. */
/* A field's type resolves from the file that wrote it, so it names what that file imports. */
static void a_field_type_names_what_its_own_file_imports(void) {
    const char *object = scratch("shapes.o");
    const char *interface = scratch("shapes.gabi");

    assert(compile("module shapes;\nstruct Point { x: i32, y: i32 }\n", object, interface, NULL));

    const char *user = scratch("field_import.o");

    const char *parts[2] = {"module holder;\nimport shapes;\nstruct Holder { at: Point }\n",
                            "module holder;\nfunc main(): i32 { return 0; }\n"};

    assert(compile_all(parts, 2, user, NULL, GAB_TEST_SCRATCH));
}

static void a_qualifier_names_the_module_that_declares_it(void) {
    const char *object = scratch("holds.o");
    const char *interface = scratch("holds.gabi");

    assert(compile("module holds;\nfunc helper(): i32 { return 5; }\n", object, interface, NULL));

    const char *user = scratch("qualifies.o");

    assert(!compile("module qualifies;\nimport holds;\n"
                    "func main(): i32 { return qualifies::helper(); }\n",
                    user, NULL, GAB_TEST_SCRATCH));
}

static void a_direct_import_reached_through_another_is_still_named(void) {
    const char *under_object = scratch("under.o");
    const char *under_interface = scratch("under.gabi");

    assert(compile("module under;\nfunc value(): i32 { return 5; }\n", under_object, under_interface, NULL));

    const char *over_object = scratch("over.o");
    const char *over_interface = scratch("over.gabi");

    assert(compile("module over;\nimport under;\nfunc via(): i32 { return under::value(); }\n", over_object,
                   over_interface, GAB_TEST_SCRATCH));

    const char *user = scratch("both_use.o");

    /* 'over' is read first and states 'under', which this module imports for itself. */
    assert(compile("module use;\nimport over;\nimport under;\n"
                   "func main(): i32 { return over::via() + under::value(); }\n",
                   user, NULL, GAB_TEST_SCRATCH));
}

/* An interface and the object it was compiled from name each other, so a stale pair cannot be linked. */
static void a_stale_interface_does_not_link(void) {
    const char *object = scratch("dig.o");
    const char *interface = scratch("dig.gabi");
    const char *stale = scratch("stale/dig.gabi");

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

    const char *user = scratch("module_test_stale_user.o");
    const char *stale_directory = scratch("stale");

    /* It compiles: the interface is well formed, and only the link can see it is not the object's. */
    assert(compile("module u;\nimport dig;\nfunc main(): i32 { return dig::helper(); }\n", user, NULL,
                   stale_directory));

    const char *binary = scratch("module_test_stale_user");

    assert(!gab_link(user, (const char *const[]){object}, 1, binary));
}

/* Nothing was compiled for arguments the declaring unit never saw, so a reader instantiates the body
 * its interface carries rather than linking against a symbol that does not exist. */
static void an_imported_generic_is_instantiated_where_it_is_named(void) {
    const char *object = scratch("gen.o");
    const char *interface = scratch("gen.gabi");

    assert(compile("module gen;\nfunc same<T>(x: T): T { return x; }\n", object, interface, NULL));

    const char *user = scratch("module_test_generic.o");

    assert(compile("module use;\nimport gen;\nfunc main(): i32 { return gen::same(7); }\n", user, NULL,
                   GAB_TEST_SCRATCH));

    const char *binary = scratch("module_test_generic");

    /* The instance is a body of the reader's own, so linking finds it without the declaring object. */
    assert(gab_link(user, (const char *const[]){object}, 1, binary));
}

/* Two readers naming one generic each instantiate it, so the two objects state the same body and the
 * link takes one rather than refusing both. */
static void one_generic_instantiated_twice_links_once(void) {
    const char *object = scratch("twice.o");
    const char *interface = scratch("twice.gabi");

    assert(compile("module twice;\nfunc same<T>(x: T): T { return x; }\n", object, interface, NULL));

    const char *first = scratch("twice_one.o");
    const char *first_interface = scratch("one.gabi");

    assert(compile("module one;\nimport twice;\nfunc up(): i32 { return twice::same(1); }\n", first,
                   first_interface, GAB_TEST_SCRATCH));

    const char *user = scratch("twice_use.o");

    assert(compile("module use;\nimport twice;\nimport one;\n"
                   "func main(): i32 { return twice::same(2) + one::up(); }\n",
                   user, NULL, GAB_TEST_SCRATCH));

    const char *binary = scratch("twice_linked");

    assert(gab_link(user, (const char *const[]){object, first}, 2, binary));
}

/* A method an imported generic type owns is instantiated where it is named, as a free function is. */
static void an_imported_generic_method_is_instantiated_where_it_is_named(void) {
    const char *object = scratch("holder.o");
    const char *interface = scratch("holder.gabi");

    assert(compile("module holder;\nstruct Box<T> { value: T }\n"
                   "impl<T> Box<T> {\n    func get(self: &Self): &T { return self.value; }\n}\n",
                   object, interface, NULL));

    const char *user = scratch("holder_use.o");

    assert(compile("module use;\nimport holder;\n"
                   "func main(): i32 { let b: holder::Box<i32> = holder::Box<i32> { value: 3 };\n"
                   "                   return *b.get(); }\n",
                   user, NULL, GAB_TEST_SCRATCH));

    const char *binary = scratch("holder_linked");

    assert(gab_link(user, (const char *const[]){object}, 1, binary));
}

typedef struct {
    TestContext ctx;
    Scope *scope;
    ResolvedModule *resolved;
    ASTModule *unit;
} Reading;

/* Both readings share one string pool, as the writer and the reader of an interface do in one
 * compilation, since an id compares by the pointers interning produced. */
static void read_source(Reading *reading, TestContext *ctx, const char *source) {
    reading->ctx = test_context_reading(ctx);

    reading->scope = scope_create_kind(reading->ctx.arena, reading->ctx.global, SCOPE_MODULE);
    reading->unit = ast_module_create(reading->ctx.arena);

    bool ok = test_resolve_ir_with(&reading->ctx, reading->scope, &reading->unit, NULL, &reading->resolved,
                                   source, false);

    assert(ok);
}

static DeclId method_id_in(Reading *reading, TestContext *ctx, const char *type, const char *method) {
    const Type *owner = scope_type_lookup(reading->ctx.types, reading->resolved->declared->scope,
                                          string_from_cstr(&ctx->strings, type));

    assert(owner);

    Function *found = function_registry_find_owned(reading->resolved->functions, owner,
                                                   string_from_cstr(&ctx->strings, method));

    assert(found);
    assert(decl_id_is_set(found->decl->id));

    return found->decl->id;
}

static DeclId type_id_in(Reading *reading, TestContext *ctx, const char *name) {
    const Type *type = scope_type_lookup(reading->ctx.types, reading->resolved->declared->scope,
                                         string_from_cstr(&ctx->strings, name));

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

    const char *path = scratch("id_written.gabi");

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
    const char *first = scratch("ffi_one.o");
    const char *first_interface = scratch("ffi_one.gabi");

    assert(compile("module ffi_one;\nextern \"C\" func getpid(): i32;\n"
                   "func stop(): i32 { return getpid(); }\n",
                   first, first_interface, NULL));

    const char *second = scratch("ffi_two.o");
    const char *second_interface = scratch("ffi_two.gabi");

    assert(compile("module ffi_two;\nextern \"C\" func getpid(): i32;\n"
                   "func halt(): i32 { return getpid(); }\n",
                   second, second_interface, NULL));

    const char *user = scratch("ffi_use.o");

    assert(compile("module use;\nimport ffi_one;\nimport ffi_two;\n"
                   "func main(): i32 { return ffi_one::stop() - ffi_two::halt(); }\n",
                   user, NULL, GAB_TEST_SCRATCH));

    const char *binary = scratch("ffi_linked");

    assert(gab_link(user, (const char *const[]){first, second}, 2, binary));
}

/* A 'main' that names no return type still marks its module executable, since C's 'main' returns
 * an int of its own and a void entry point does not need to supply one. */
static void a_void_main_is_an_executable_entry_point(void) {
    const char *object = scratch("void_main.o");

    assert(compile("module void_main;\nfunc main() { }\n", object, NULL, NULL));

    const char *binary = scratch("void_main");

    assert(gab_link(object, NULL, 0, binary));

    assert(system(binary) == 0);
}

/* A 'main' promising neither i32 nor nothing names no entry point an executable can call. */
static void a_main_that_returns_neither_i32_nor_nothing_is_not_an_entry_point(void) {
    const char *object = scratch("bool_main.o");

    assert(!compile("module bool_main;\nfunc main(): bool { return true; }\n", object, NULL, NULL));
}

int main(void) {
    a_main_that_returns_neither_i32_nor_nothing_is_not_an_entry_point();
    a_void_main_is_an_executable_entry_point();
    two_modules_declare_one_foreign_function();
    a_declaration_read_back_has_the_id_it_was_written_with();
    an_imported_generic_method_is_instantiated_where_it_is_named();
    one_generic_instantiated_twice_links_once();
    an_imported_generic_is_instantiated_where_it_is_named();
    a_unit_names_what_an_imported_interface_declares();
    a_module_is_named_only_where_it_is_imported();
    an_import_is_named_only_in_the_file_that_imports_it();
    a_field_names_a_type_its_own_file_imports();
    an_interface_states_an_import_once();
    a_declaration_is_named_across_the_files_of_its_module();
    a_name_two_files_declare_is_declared_twice();
    a_module_declares_a_name_the_prelude_holds();
    a_declaration_does_not_take_the_name_of_an_import();
    a_field_type_names_what_its_own_file_imports();
    a_qualifier_names_the_module_that_declares_it();
    a_direct_import_reached_through_another_is_still_named();
    a_module_that_imports_itself_is_an_error();
    a_stale_interface_does_not_link();
    a_module_is_written_across_the_files_it_is_compiled_from();
    every_file_of_a_module_declares_that_module();

    printf("module tests passed\n");

    return 0;
}
