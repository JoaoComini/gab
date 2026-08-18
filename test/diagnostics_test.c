#include "arena.h"
#include "ast/ast.h"
#include "diagnostics.h"
#include "lexer.h"
#include "parser.h"
#include "scope.h"
#include "string/string.h"
#include "support/test_context.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Runs the full front end (parse + resolve) over a source string. Diagnostics
// land in the context's sink, whose arena keeps the messages alive.
static void compile(TestContext *ctx, const char *source) {
    Arena *arena = ctx->arena;
    Diagnostics *diagnostics = &ctx->diagnostics;

    Lexer lexer = lexer_create(source, diagnostics);
    Parser parser = parser_create(&lexer, diagnostics);
    ASTScript *script = ast_script_create();

    Scope global_scope;
    scope_init(&global_scope, arena, &ctx->strings, NULL);

    if (parser_parse(&parser, script)) {
        ast_script_resolve(arena, script, &global_scope, diagnostics);
    }

    ast_script_destroy(script);
}

static void test_records_kind_and_position() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    diag_error(diagnostics, GAB_ERR_TYPE, (Span){.line = 3, .column = 7}, "expected %s, found %s", "int",
               "float");

    assert(diagnostics_has_errors(diagnostics));
    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(diagnostic->span.line == 3);
    assert(diagnostic->span.column == 7);
    assert(strcmp(diagnostic->message, "expected int, found float") == 0);

    test_context_free(&ctx);
}

static void test_empty_sink_has_no_errors() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    assert(!diagnostics_has_errors(diagnostics));
    assert(diagnostics_count(diagnostics) == 0);
    assert(diagnostics_get(diagnostics, 0) == NULL);

    test_context_free(&ctx);
}

static void test_accumulates_in_source_order() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    diag_error(diagnostics, GAB_ERR_NAME, (Span){.line = 1, .column = 1}, "first");
    diag_error(diagnostics, GAB_ERR_TYPE, (Span){.line = 2, .column = 1}, "second");
    diag_error(diagnostics, GAB_ERR_SYNTAX, (Span){.line = 3, .column = 1}, "third");

    assert(diagnostics_count(diagnostics) == 3);
    assert(strcmp(diagnostics_get(diagnostics, 0)->message, "first") == 0);
    assert(strcmp(diagnostics_get(diagnostics, 1)->message, "second") == 0);
    assert(strcmp(diagnostics_get(diagnostics, 2)->message, "third") == 0);

    assert(diagnostics_get(diagnostics, 0)->span.line == 1);
    assert(diagnostics_get(diagnostics, 1)->span.line == 2);
    assert(diagnostics_get(diagnostics, 2)->span.line == 3);

    test_context_free(&ctx);
}

// The point of the poison type: one root cause yields one diagnostic, not one
// per check that later touches the poisoned value.
static void test_poison_suppresses_cascades() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: int = undefined_var + 1; }");

    if (diagnostics_count(diagnostics) != 1) {
        diagnostics_print(diagnostics, stderr);
    }

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_NAME);
    assert(strcmp(diagnostic->message, "undeclared variable 'undefined_var'") == 0);

    test_context_free(&ctx);
}

static void test_reports_multiple_semantic_errors() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    // Two independent undeclared variables: resolution keeps going after the
    // first, so both are reported.
    compile(&ctx, "func test() { let a: int = first; let b: int = second; }");

    assert(diagnostics_count(diagnostics) == 2);
    assert(strcmp(diagnostics_get(diagnostics, 0)->message, "undeclared variable 'first'") == 0);
    assert(strcmp(diagnostics_get(diagnostics, 1)->message, "undeclared variable 'second'") == 0);

    test_context_free(&ctx);
}

static void test_reports_type_mismatch() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: int = 1.5; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message,
                  "cannot initialize a variable of type int with a value of type float") == 0);

    test_context_free(&ctx);
}

static void test_reports_unknown_type() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let v: Vec3 = 1; }");

    // The unknown type poisons the declaration, so the initializer mismatch
    // that follows is suppressed.
    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_NAME);
    assert(strcmp(diagnostic->message, "unknown type 'Vec3'") == 0);

    test_context_free(&ctx);
}

static void test_reports_duplicate_declaration() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: int = 1; let x: int = 2; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_NAME);
    assert(strcmp(diagnostic->message, "'x' is already declared in this scope") == 0);

    test_context_free(&ctx);
}

static void test_reports_unknown_field_type() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Broken { value: Nope }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_NAME);
    assert(strcmp(diagnostic->message, "unknown type 'Nope'") == 0);

    test_context_free(&ctx);
}

static void test_reports_unknown_field_name() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Vec3 { x: float, y: float, z: float }\n"
                  "func f(): float { let v: Vec3; return v.w; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_NAME);
    assert(strcmp(diagnostic->message, "'Vec3' has no field 'w'") == 0);

    test_context_free(&ctx);
}

static void test_reports_field_access_on_a_non_struct() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func f(): int { let n: int = 1; return n.x; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "int is not a struct, so it has no fields") == 0);

    test_context_free(&ctx);
}

static void test_reports_mismatched_field_assignment() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Vec3 { x: float, y: float, z: float }\n"
                  "func f(): float { let v: Vec3; v.x = true; return v.x; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot assign a value of type bool to a target of type float") == 0);

    test_context_free(&ctx);
}

static void test_reports_duplicate_field() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Broken { value: int, value: float }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_NAME);
    assert(strcmp(diagnostic->message, "duplicate field 'value' in struct 'Broken'") == 0);

    test_context_free(&ctx);
}

static void test_reports_duplicate_struct_name() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Vec2 { x: float } struct Vec2 { y: float }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_NAME);
    assert(strcmp(diagnostic->message, "type 'Vec2' is already declared") == 0);

    test_context_free(&ctx);
}

// Declaring a struct named after a builtin would shadow it for every later
// resolution, so it is rejected by the same duplicate check.
static void test_rejects_shadowing_a_builtin() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct int { x: float }");

    assert(diagnostics_count(diagnostics) == 1);
    assert(strcmp(diagnostics_get(diagnostics, 0)->message, "type 'int' is already declared") == 0);

    test_context_free(&ctx);
}

// Without an explicit check this would surface as "unknown type 'Node'", since
// the struct is registered only after its fields resolve.
static void test_reports_self_referential_struct() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Node { next: Node }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "struct 'Node' cannot contain itself") == 0);

    test_context_free(&ctx);
}

static void test_reports_every_bad_field() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Broken { a: Nope, b: AlsoNope }");

    assert(diagnostics_count(diagnostics) == 2);
    assert(strcmp(diagnostics_get(diagnostics, 0)->message, "unknown type 'Nope'") == 0);
    assert(strcmp(diagnostics_get(diagnostics, 1)->message, "unknown type 'AlsoNope'") == 0);

    test_context_free(&ctx);
}

static void test_reports_wrong_argument_count() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func f(a: int): int { return a; } func g(): int { return f(1, 2); }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "expected 1 argument(s), found 2") == 0);

    test_context_free(&ctx);
}

static void test_reports_wrong_argument_type() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func f(a: int): int { return a; } func g(): int { return f(1.5); }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "argument 1 is float, but int was declared") == 0);

    test_context_free(&ctx);
}

static void test_reports_calling_a_non_function() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func g(): int { let x: int = 1; return x(); }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "this expression is not callable") == 0);

    test_context_free(&ctx);
}

// Statement-level recovery: without it the parser would stop at the first bad
// statement and the second would never be reported.
static void test_parser_recovers_across_statements() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let ; let ; }");

    assert(diagnostics_count(diagnostics) == 2);
    assert(strcmp(diagnostics_get(diagnostics, 0)->message,
                  "expected a variable name after 'let', found ';'") == 0);
    assert(strcmp(diagnostics_get(diagnostics, 1)->message,
                  "expected a variable name after 'let', found ';'") == 0);

    test_context_free(&ctx);
}

// Syntax errors name the offending token, not just what was expected.
static void test_syntax_errors_name_the_found_token() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: int = 1 }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_SYNTAX);
    assert(strcmp(diagnostic->message, "expected ';', found '}'") == 0);

    test_context_free(&ctx);
}

static void test_lexer_reports_invalid_character() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    Lexer lexer = lexer_create("let x = $;", diagnostics);
    while (lexer_next(&lexer).type != TOKEN_EOF) {
    }

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_SYNTAX);
    assert(strcmp(diagnostic->message, "unexpected character '$'") == 0);
    assert(diagnostic->span.line == 1);
    assert(diagnostic->span.column == 9);

    test_context_free(&ctx);
}

// Positions must survive newlines, or multi-line scripts report nonsense.
static void test_spans_track_lines_and_columns() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() {\n    let x: int = 1;\n    let y: int = nope;\n}");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(strcmp(diagnostic->message, "undeclared variable 'nope'") == 0);
    assert(diagnostic->span.line == 3);
    assert(diagnostic->span.column == 18);

    test_context_free(&ctx);
}

int main(void) {

    test_empty_sink_has_no_errors();
    test_records_kind_and_position();
    test_accumulates_in_source_order();

    test_poison_suppresses_cascades();
    test_reports_multiple_semantic_errors();
    test_reports_type_mismatch();
    test_reports_unknown_type();
    test_reports_duplicate_declaration();

    test_reports_unknown_field_type();
    test_reports_unknown_field_name();
    test_reports_field_access_on_a_non_struct();
    test_reports_mismatched_field_assignment();
    test_reports_duplicate_field();
    test_reports_duplicate_struct_name();
    test_rejects_shadowing_a_builtin();
    test_reports_self_referential_struct();
    test_reports_every_bad_field();

    test_reports_wrong_argument_count();
    test_reports_wrong_argument_type();
    test_reports_calling_a_non_function();

    test_parser_recovers_across_statements();
    test_syntax_errors_name_the_found_token();
    test_lexer_reports_invalid_character();
    test_spans_track_lines_and_columns();

    printf("All diagnostics tests passed\n");
    return 0;
}
