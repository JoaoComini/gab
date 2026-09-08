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

/* A generic is instantiated by whoever names it, so what it declares must carry the body to instantiate. */
static void an_interface_carries_the_body_of_a_generic(void) {
    char *text = interface_of("struct Pair<T> { a: T, b: T }\n"
                              "func first<T>(p: &Pair<T>, take: bool): T {\n"
                              "    if take { return p.a; }\n"
                              "    for let i: i32 = 0; i < 2; i = i + 1 { }\n"
                              "    return p.b;\n"
                              "}\n"
                              "func plain(x: i32): i32 { return x + 1; }\n");

    assert(strstr(text, "func first<T>(p: &Pair<T>, take: bool): T {"));
    assert(strstr(text, "return p.a;"));

    /* One the declaring unit compiled is linked against rather than instantiated again. */
    assert(strstr(text, "extern func plain(x: i32): i32;"));

    /* What carries a body states the same thing when it is read back, as a signature does. */
    char *again = interface_of(text);

    assert(strcmp(text, again) == 0);

    free(again);

    free(text);
}

/* Every shape a body can hold reaches a reader unchanged, since what is read back is what is compiled. */
static void a_carried_body_states_every_shape_it_holds(void) {
    char *text =
        interface_of("struct Holder<T> { value: T, flag: bool }\n"
                     "func shapes<T>(h: &Holder<T>, xs: &array<i32, 4>, n: i32): i32 {\n"
                     "    let total: i32 = 0;\n"
                     "    let pair: array<i32, 2> = [1, 2];\n"
                     "    let made: Holder<i32> = Holder<i32> { value: 3, flag: !h.flag };\n"
                     "    let owned: *i32 = box n;\n"
                     "    let seen: &i32 = total;\n"
                     "    total = ((-n * 2) + 5) % 7;\n"
                     "    total = total - xs[0] / 2;\n"
                     "    if h.flag && n > 0 || n <= -1 { total = *owned; } else { total = made.value; }\n"
                     "    for { break; }\n"
                     "    for n != 0 { continue; }\n"
                     "    return total + pair[1] + *seen;\n"
                     "}\n");

    char *again = interface_of(text);

    assert(strcmp(text, again) == 0);

    free(again);
    free(text);
}

int main(void) {
    a_carried_body_states_every_shape_it_holds();
    an_interface_carries_the_body_of_a_generic();
    an_interface_states_the_same_thing_when_read_back();
    a_written_interface_compiles();
    an_interface_states_a_signature_without_its_body();
    a_method_does_not_restate_the_parameters_of_its_block();

    printf("interface tests passed\n");

    return 0;
}
