#include "driver/interface.h"

#include "support/run.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GAB_TEST_SCRATCH
#define GAB_TEST_SCRATCH "."
#endif

/* The core's own source, which is what an interface is written from. */
static char *core_source(void) {
    char *text = gab_interface_read(GAB_TEST_CORE_SOURCE);

    assert(text);

    return text;
}

/* The interface a source states, written and read back as the next compilation would find it. */
static char *interface_of(const char *source) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create(ctx.arena);

    bool ok = test_resolve_ir_with(&ctx, scope, &unit, NULL, NULL, source, true);
    assert(ok);

    char path[512];
    snprintf(path, sizeof(path), "%s/interface_test.gabi", GAB_TEST_SCRATCH);

    assert(gab_interface_write(unit, path));

    char *text = gab_interface_read(path);
    assert(text);

    test_context_free(&ctx);

    return text;
}

static void a_written_interface_compiles(void) {
    char *source = core_source();
    char *text = interface_of(source);

    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create(ctx.arena);

    assert(test_resolve_ir_with(&ctx, scope, &unit, NULL, NULL, text, true));

    test_context_free(&ctx);
    free(text);
    free(source);
}

static void an_interface_states_a_signature_without_its_body(void) {
    char *source = core_source();
    char *text = interface_of(source);

    assert(strstr(text, "func len(self: &str): i32;"));
    assert(!strstr(text, "as_bytes().len()"));

    free(text);
    free(source);
}

static void a_method_does_not_restate_the_parameters_of_its_block(void) {
    char *source = core_source();
    char *text = interface_of(source);

    assert(strstr(text, "impl<T> slice<T> {"));
    assert(!strstr(text, "func len<T>"));

    free(text);
    free(source);
}

/* What a reader parses states exactly what was written, so a body carried across is carried unchanged. */
static void an_interface_states_the_same_thing_when_read_back(void) {
    char *source = core_source();
    char *once = interface_of(source);
    char *twice = interface_of(once);

    assert(strcmp(once, twice) == 0);

    free(twice);
    free(once);
    free(source);
}

int main(void) {
    an_interface_states_the_same_thing_when_read_back();
    a_written_interface_compiles();
    an_interface_states_a_signature_without_its_body();
    a_method_does_not_restate_the_parameters_of_its_block();

    printf("interface tests passed\n");

    return 0;
}
