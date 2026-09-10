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

static void an_import_is_named_only_in_the_file_that_imports_it(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/perfile.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/perfile.gabi", GAB_TEST_SCRATCH);

    assert(compile("module perfile;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/perfile_use.o", GAB_TEST_SCRATCH);

    const char *parts[2] = {"module use;\nimport perfile;\nfunc one(): i32 { return perfile::helper(); }\n",
                            "module use;\nfunc two(): i32 { return perfile::helper(); }\n"};

    assert(!compile_all(parts, 2, user, NULL, GAB_TEST_SCRATCH));
}

/* A field's type is resolved after every file has declared, so it reads the imports of its own file. */
static void a_field_names_a_type_its_own_file_imports(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/fieldlib.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/fieldlib.gabi", GAB_TEST_SCRATCH);

    assert(compile("module fieldlib;\nstruct Held { value: i32, }\n", object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/fieldlib_use.o", GAB_TEST_SCRATCH);

    const char *parts[2] = {"module use;\nimport fieldlib;\nstruct Wrap { held: fieldlib::Held, }\n",
                            "module use;\nfunc main(): i32 { return 0; }\n"};

    assert(compile_all(parts, 2, user, NULL, GAB_TEST_SCRATCH));
}

/* An interface states the module, so a module names an import once however many of its files import it. */
static void an_interface_states_an_import_once(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/shared.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/shared.gabi", GAB_TEST_SCRATCH);

    assert(compile("module shared;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    char user[512];
    char stated[512];

    snprintf(user, sizeof(user), "%s/twice.o", GAB_TEST_SCRATCH);
    snprintf(stated, sizeof(stated), "%s/twice.gabi", GAB_TEST_SCRATCH);

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
    char object[512];
    snprintf(object, sizeof(object), "%s/across.o", GAB_TEST_SCRATCH);

    const char *parts[2] = {"module across;\nfunc one(): i32 { return 1; }\n",
                            "module across;\nfunc two(): i32 { return one() + 1; }\n"};

    assert(compile_all(parts, 2, object, NULL, NULL));
}

/* A module and its files are one namespace, so a name one file declares collides with the other's. */
static void a_name_two_files_declare_is_declared_twice(void) {
    char object[512];
    snprintf(object, sizeof(object), "%s/collide.o", GAB_TEST_SCRATCH);

    const char *parts[2] = {"module collide;\nfunc same(): i32 { return 1; }\n",
                            "module collide;\nfunc same(): i32 { return 2; }\n"};

    assert(!compile_all(parts, 2, object, NULL, NULL));
}

/* The prelude declares into the global scope, which a module's own declaration shadows. */
static void a_module_declares_a_name_the_prelude_holds(void) {
    char object[512];
    snprintf(object, sizeof(object), "%s/shadows.o", GAB_TEST_SCRATCH);

    assert(compile("module shadows;\nstruct Location { x: i32, }\n"
                   "func main(): i32 { let l = Location { x: 5 }; return l.x; }\n",
                   object, NULL, NULL));
}

/* An import binds the module in the file that wrote it, so a declaration cannot take the same name. */
static void a_declaration_does_not_take_the_name_of_an_import(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/taken.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/taken.gabi", GAB_TEST_SCRATCH);

    assert(compile("module taken;\nfunc helper(): i32 { return 7; }\n", object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/taken_use.o", GAB_TEST_SCRATCH);

    assert(!compile("module use;\nimport taken;\nfunc taken(): i32 { return 1; }\n", user, NULL,
                    GAB_TEST_SCRATCH));
}

/* A module imported directly and reached through another as well is still one this module may name. */
static void an_import_is_read_before_whoever_imports_it(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/deep.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/deep.gabi", GAB_TEST_SCRATCH);

    assert(compile("module deep;\nstruct Cell { value: i32 }\n", object, interface, NULL));

    snprintf(object, sizeof(object), "%s/middle.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/middle.gabi", GAB_TEST_SCRATCH);

    assert(compile("module middle;\nimport deep;\nfunc hold(): deep::Cell { return deep::Cell{value: 7}; }\n",
                   object, interface, GAB_TEST_SCRATCH));

    char user[512];
    snprintf(user, sizeof(user), "%s/deep_use.o", GAB_TEST_SCRATCH);

    assert(compile("module use;\nimport middle;\n"
                   "func main(): i32 { return middle::hold().value; }\n",
                   user, NULL, GAB_TEST_SCRATCH));
}

static void a_module_that_imports_itself_is_an_error(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/loop.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/loop.gabi", GAB_TEST_SCRATCH);

    assert(compile("module loop;\nfunc value(): i32 { return 1; }\n", object, interface, NULL));

    /* Restated so the interface imports itself, which no compilation of the source could write. */
    write_file(interface, "module loop;\nimport loop;\nfunc value(): i32;\n");

    char user[512];
    snprintf(user, sizeof(user), "%s/loop_use.o", GAB_TEST_SCRATCH);

    assert(!compile("module use;\nimport loop;\nfunc main(): i32 { return loop::value(); }\n", user, NULL,
                    GAB_TEST_SCRATCH));
}

/* A name is qualified by the module that declares it, so one module's name does not reach another's. */
/* A field's type resolves from the file that wrote it, so it names what that file imports. */
static void a_field_type_names_what_its_own_file_imports(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/shapes.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/shapes.gabi", GAB_TEST_SCRATCH);

    assert(compile("module shapes;\nstruct Point { x: i32, y: i32 }\n", object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/field_import.o", GAB_TEST_SCRATCH);

    const char *parts[2] = {"module holder;\nimport shapes;\nstruct Holder { at: Point }\n",
                            "module holder;\nfunc main(): i32 { return 0; }\n"};

    assert(compile_all(parts, 2, user, NULL, GAB_TEST_SCRATCH));
}

static void a_qualifier_names_the_module_that_declares_it(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/holds.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/holds.gabi", GAB_TEST_SCRATCH);

    assert(compile("module holds;\nfunc helper(): i32 { return 5; }\n", object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/qualifies.o", GAB_TEST_SCRATCH);

    assert(!compile("module qualifies;\nimport holds;\n"
                    "func main(): i32 { return qualifies::helper(); }\n",
                    user, NULL, GAB_TEST_SCRATCH));
}

static void a_direct_import_reached_through_another_is_still_named(void) {
    char object[512];
    char interface[512];

    snprintf(object, sizeof(object), "%s/under.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/under.gabi", GAB_TEST_SCRATCH);

    assert(compile("module under;\nfunc value(): i32 { return 5; }\n", object, interface, NULL));

    snprintf(object, sizeof(object), "%s/over.o", GAB_TEST_SCRATCH);
    snprintf(interface, sizeof(interface), "%s/over.gabi", GAB_TEST_SCRATCH);

    assert(compile("module over;\nimport under;\nfunc via(): i32 { return under::value(); }\n", object,
                   interface, GAB_TEST_SCRATCH));

    char user[512];
    snprintf(user, sizeof(user), "%s/both_use.o", GAB_TEST_SCRATCH);

    /* 'over' is read first and states 'under', which this module imports for itself. */
    assert(compile("module use;\nimport over;\nimport under;\n"
                   "func main(): i32 { return over::via() + under::value(); }\n",
                   user, NULL, GAB_TEST_SCRATCH));
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
                   "impl<T> Box<T> {\n    func get(self: &Self): &T { return self.value; }\n}\n",
                   object, interface, NULL));

    char user[512];
    snprintf(user, sizeof(user), "%s/holder_use.o", GAB_TEST_SCRATCH);

    assert(compile("module use;\nimport holder;\n"
                   "func main(): i32 { let b: holder::Box<i32> = holder::Box<i32> { value: 3 };\n"
                   "                   return *b.get(); }\n",
                   user, NULL, GAB_TEST_SCRATCH));

    char binary[512];
    snprintf(binary, sizeof(binary), "%s/holder_linked", GAB_TEST_SCRATCH);

    assert(gab_link(user, "use", (const char *const[]){object}, 1, binary));
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
