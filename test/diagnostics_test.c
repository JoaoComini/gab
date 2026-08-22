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

    Lexer lexer = lexer_create(test_in_a_module(source), ctx->arena, &ctx->strings, diagnostics);
    Parser parser = parser_create(&lexer, diagnostics);
    ASTScript *script = ast_script_create();

    Scope global_scope;
    scope_init(&global_scope, arena, &ctx->strings, NULL);

    if (parser_parse(&parser, script)) {
        ast_script_resolve(arena, script, &global_scope, NULL, diagnostics);
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

// A binary operator names itself and the type it was given, so the message
// says which rule was broken rather than that something was wrong.
static void test_reports_a_bad_binary_operand() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): int { let a: bool = true; let b: bool = false; return a - b; }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "'-' requires a numeric type, found bool") == 0);

    test_context_free(&ctx);
}

// A compound assignment reports against the bare operator, which is the rule
// it actually inherits: '%=' is refused because '%' is int-only.
static void test_reports_a_bad_compound_assignment_operand() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): float { let a: float = 1.0; a %= 2.0; return a; }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "'%' requires an integer type, found float") == 0);

    test_context_free(&ctx);
}

// Mismatched sides name the compound spelling, since that is what was written.
static void test_reports_a_mismatched_compound_assignment() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): int { let a: int = 1; a += 2.0; return a; }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot apply '+=' to int and float") == 0);

    test_context_free(&ctx);
}

// A conversion the language does not have names both types, so the message
// says what was asked for rather than that a call went wrong.
static void test_reports_an_illegal_conversion() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): int { let a: bool = true; return int(a); }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot convert bool to int") == 0);

    test_context_free(&ctx);
}

// A conversion reports as a conversion, not as a call with the wrong number of
// arguments: 'int' never named a function.
static void test_reports_a_conversion_with_the_wrong_operand_count() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): int { return int(); }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "a conversion to int takes one operand") == 0);

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

// A builtin is reachable from every module, and nothing can name it past a
// shadow, so declaring a struct over one is refused rather than allowed to hide
// it for the rest of the unit.
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

    Lexer lexer = lexer_create("let x = $;", ctx.arena, &ctx.strings, diagnostics);
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

    // Writes its own directive, so the line numbers asserted below are the ones
    // in the string rather than the ones after a helper prepended anything.
    compile(&ctx, "module test;\nfunc test() {\n    let x: int = 1;\n    let y: int = nope;\n}");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(strcmp(diagnostic->message, "undeclared variable 'nope'") == 0);
    assert(diagnostic->span.line == 4);
    assert(diagnostic->span.column == 18);

    test_context_free(&ctx);
}

static void test_reports_address_of_a_temporary() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let p: ref int = &1; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot take the address of a temporary") == 0);

    test_context_free(&ctx);
}

// A call returns a value with no home in memory, so there is no address of it
// to take either.
static void test_reports_address_of_a_call_result() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func g(): int { return 1; }\nfunc test() { let p: ref int = &g(); }");

    assert(diagnostics_count(diagnostics) == 1);
    assert(strcmp(diagnostics_get(diagnostics, 0)->message, "cannot take the address of a temporary") == 0);

    test_context_free(&ctx);
}

static void test_reports_dereferencing_a_non_pointer() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: int = 1; let y: int = *x; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot dereference int") == 0);

    test_context_free(&ctx);
}

// Field access reaches through one pointer, so a pointer to a non-struct still
// has no fields — and the message names what was written, not the pointee.
static void test_reports_field_access_through_a_non_struct_pointer() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: int = 1; let p: ref int = &x; let y: int = p.field; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "ref int is not a struct, so it has no fields") == 0);

    test_context_free(&ctx);
}

// A returned pointer outlives the whole frame, so nothing declared inside the
// function may be pointed at.
static void test_reports_returning_a_pointer_to_a_local() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): ref int { let x: int = 1; return &x; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_LIFETIME);
    assert(strcmp(diagnostic->message, "this pointer outlives what it points at, so it cannot be returned") ==
           0);

    test_context_free(&ctx);
}

// Register reuse reclaims slots at the closing brace, so a pointer into an
// inner block dangles into a reused slot the moment that block ends. The rule
// is block-scoped for exactly this reason.
static void test_reports_a_pointer_escaping_its_block() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let p: ref int; { let x: int = 1; p = &x; } }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_LIFETIME);
    assert(strcmp(diagnostic->message,
                  "this pointer outlives what it points at, so it cannot be assigned here") == 0);

    test_context_free(&ctx);
}

// A heap object outlives every frame, so storing a stack address into one is
// the escape the rule exists to catch: nothing can save a pointer whose pointee
// is a frame slot that has already gone.
static void test_reports_a_stack_pointer_stored_into_a_heap_object() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Inner { n: int }\n"
                  "struct Outer { child: ref Inner }\n"
                  "func test() { let o: *Outer = new Outer; let local: Inner; o.child = &local; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_LIFETIME);
    assert(strcmp(diagnostic->message,
                  "this pointer outlives what it points at, so it cannot be stored here") == 0);

    test_context_free(&ctx);
}

// The restriction is only on outliving the pointee. A pointer that stays at or
// below its pointee's depth is fine, including one passed to a callee.
static void test_accepts_pointers_that_do_not_outlive_their_pointee() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "func take(p: ref int): int { return *p; }\n"
                  "func test(): int {\n"
                  "let x: int = 1;\n"
                  "let p: ref int = &x;\n"
                  "{ let inner: ref int = &x; }\n"
                  "return take(&x) + *p;\n"
                  "}");

    assert(diagnostics_count(&ctx.diagnostics) == 0);

    test_context_free(&ctx);
}

int main(void) {

    test_empty_sink_has_no_errors();
    test_records_kind_and_position();
    test_accumulates_in_source_order();

    test_poison_suppresses_cascades();
    test_reports_multiple_semantic_errors();
    test_reports_type_mismatch();
    test_reports_a_bad_binary_operand();
    test_reports_a_bad_compound_assignment_operand();
    test_reports_a_mismatched_compound_assignment();
    test_reports_an_illegal_conversion();
    test_reports_a_conversion_with_the_wrong_operand_count();
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

    test_reports_address_of_a_temporary();
    test_reports_address_of_a_call_result();
    test_reports_dereferencing_a_non_pointer();
    test_reports_field_access_through_a_non_struct_pointer();
    test_reports_returning_a_pointer_to_a_local();
    test_reports_a_pointer_escaping_its_block();
    test_reports_a_stack_pointer_stored_into_a_heap_object();
    test_accepts_pointers_that_do_not_outlive_their_pointee();

    test_parser_recovers_across_statements();
    test_syntax_errors_name_the_found_token();
    test_lexer_reports_invalid_character();
    test_spans_track_lines_and_columns();

    printf("All diagnostics tests passed\n");
    return 0;
}
