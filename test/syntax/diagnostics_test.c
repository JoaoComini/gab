#include "ast/resolve.h"
#include "diagnostics.h"
#include "memory/arena.h"
#include "mir/mir_build.h"
#include "scope.h"
#include "string/string.h"
#include "support/test_context.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void compile(TestContext *ctx, const char *source) {
    Arena *arena = ctx->arena;
    Diagnostics *diagnostics = &ctx->diagnostics;

    Scope global_scope;
    scope_init(&global_scope, arena, &ctx->strings, NULL);

    ASTModule *unit;
    ResolvedModule *resolved;
    MIRModule *mir_unit;

    if (parse_module((const char *const[]){test_in_a_module(source)}, 1, NULL, ctx->arena, &ctx->strings,
                     &unit, diagnostics) &&
        resolve_module(arena, unit, &global_scope, NULL, false, &resolved, diagnostics)) {
        mir_build(arena, resolved, NULL, &mir_unit, diagnostics);
    }
}

static void test_a_struct_local_without_a_literal_reports_once() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "struct Point { x: i32, y: i32 }\n"
                  "func f(): i32 { let v: Point; v.x = 1; return v.x + v.y; }\n");

    assert(diagnostics_count(&ctx.diagnostics) == 1);

    test_context_free(&ctx);
}

static void test_records_kind_and_position() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    diag_error(diagnostics, GAB_ERR_TYPE, (Span){.line = 3, .column = 7}, "expected %s, found %s", "i32",
               "f32");

    assert(diagnostics_has_errors(diagnostics));
    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(diagnostic->span.line == 3);
    assert(diagnostic->span.column == 7);
    assert(strcmp(diagnostic->message, "expected i32, found f32") == 0);

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

static void test_poison_suppresses_cascades() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: i32 = undefined_var + 1; }");

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

    compile(&ctx, "func test() { let a: i32 = first; let b: i32 = second; }");

    assert(diagnostics_count(diagnostics) == 2);
    assert(strcmp(diagnostics_get(diagnostics, 0)->message, "undeclared variable 'first'") == 0);
    assert(strcmp(diagnostics_get(diagnostics, 1)->message, "undeclared variable 'second'") == 0);

    test_context_free(&ctx);
}

static void test_reports_type_mismatch() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: i32 = 1.5; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot initialize a variable of type i32 with a value of type f32") ==
           0);

    test_context_free(&ctx);
}

static void test_reports_a_bad_binary_operand() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): i32 { let a: bool = true; let b: bool = false; return a - b; }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "'-' requires a numeric type, found bool") == 0);

    test_context_free(&ctx);
}

static void test_reports_a_bad_compound_assignment_operand() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): f32 { let a: f32 = 1.0; a %= 2.0; return a; }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "'%' requires an integer type, found f32") == 0);

    test_context_free(&ctx);
}

static void test_reports_a_mismatched_compound_assignment() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): i32 { let a: i32 = 1; a += 2.0; return a; }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot apply '+=' to i32 and f32") == 0);

    test_context_free(&ctx);
}

static void test_reports_an_illegal_conversion() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): i32 { let a: bool = true; return i32(a); }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot convert bool to i32") == 0);

    test_context_free(&ctx);
}

static void test_reports_a_conversion_with_the_wrong_operand_count() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): i32 { return i32(); }");

    assert(diagnostics_count(diagnostics) >= 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "a conversion to i32 takes one operand") == 0);

    test_context_free(&ctx);
}

static void test_reports_unknown_type() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let v: Vec3 = 1; }");

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

    compile(&ctx, "func test() { let x: i32 = 1; let x: i32 = 2; }");

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

    compile(&ctx, "struct Vec3 { x: f32, y: f32, z: f32 }\n"
                  "func f(): f32 { let v = Vec3 { x: 0.0, y: 0.0, z: 0.0 }; return v.w; }");

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

    compile(&ctx, "func f(): i32 { let n: i32 = 1; return n.x; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "i32 is not a struct, so it has no fields") == 0);

    test_context_free(&ctx);
}

static void test_reports_mismatched_field_assignment() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Vec3 { x: f32, y: f32, z: f32 }\n"
                  "func f(): f32 { let v = Vec3 { x: 0.0, y: 0.0, z: 0.0 }; v.x = true; return v.x; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot assign a value of type bool to a target of type f32") == 0);

    test_context_free(&ctx);
}

static void test_reports_duplicate_field() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Broken { value: i32, value: f32 }");

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

    compile(&ctx, "struct Vec2 { x: f32 } struct Vec2 { y: f32 }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_NAME);
    assert(strcmp(diagnostic->message, "type 'Vec2' is already declared") == 0);

    test_context_free(&ctx);
}

static void test_rejects_shadowing_a_builtin() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct i32 { x: f32 }");

    assert(diagnostics_count(diagnostics) == 1);
    assert(strcmp(diagnostics_get(diagnostics, 0)->message, "type 'i32' is already declared") == 0);

    test_context_free(&ctx);
}

static void test_reports_self_referential_struct() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Node { next: Node }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "struct 'Node' cannot contain itself: 'Node' contains 'Node'") == 0);

    test_context_free(&ctx);
}

static void test_reports_mutual_containment_cycle() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct A { b: B }\n"
                  "struct B { a: A }\n");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "struct 'A' cannot contain itself: 'A' contains 'B' contains 'A'") ==
           0);

    test_context_free(&ctx);
}

static void test_reports_the_path_a_containment_ring_takes() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "struct A { b: B }\n"
                  "struct B { c: C }\n"
                  "struct C { a: A }\n");

    assert(diagnostics_count(&ctx.diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(&ctx.diagnostics, 0);

    assert(strcmp(diagnostic->message,
                  "struct 'A' cannot contain itself: 'A' contains 'B' contains 'C' contains 'A'") == 0);

    assert(diagnostic->span.line == 4);

    test_context_free(&ctx);
}

static void test_a_second_ring_traces_only_its_own_path() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "struct A { b: B }\n"
                  "struct B { a: A }\n"
                  "struct C { d: D }\n"
                  "struct D { c: C }\n");

    assert(diagnostics_count(&ctx.diagnostics) == 2);

    assert(strcmp(diagnostics_get(&ctx.diagnostics, 0)->message,
                  "struct 'A' cannot contain itself: 'A' contains 'B' contains 'A'") == 0);

    assert(strcmp(diagnostics_get(&ctx.diagnostics, 1)->message,
                  "struct 'C' cannot contain itself: 'C' contains 'D' contains 'C'") == 0);

    test_context_free(&ctx);
}

static void test_a_ring_reached_from_outside_names_only_the_ring() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "struct X { a: A }\n"
                  "struct A { b: B }\n"
                  "struct B { a: A }\n");

    assert(diagnostics_count(&ctx.diagnostics) == 1);

    assert(strcmp(diagnostics_get(&ctx.diagnostics, 0)->message,
                  "struct 'A' cannot contain itself: 'A' contains 'B' contains 'A'") == 0);

    test_context_free(&ctx);
}

static void test_a_ring_too_long_to_name_is_elided_at_a_hop() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "struct S0 { f: S1 }\n"
                  "struct S1 { f: S2 }\n"
                  "struct S2 { f: S3 }\n"
                  "struct S3 { f: S4 }\n"
                  "struct S4 { f: S5 }\n"
                  "struct S5 { f: S6 }\n"
                  "struct S6 { f: S7 }\n"
                  "struct S7 { f: S8 }\n"
                  "struct S8 { f: S9 }\n"
                  "struct S9 { f: S10 }\n"
                  "struct S10 { f: S11 }\n"
                  "struct S11 { f: S12 }\n"
                  "struct S12 { f: S13 }\n"
                  "struct S13 { f: S14 }\n"
                  "struct S14 { f: S15 }\n"
                  "struct S15 { f: S16 }\n"
                  "struct S16 { f: S17 }\n"
                  "struct S17 { f: S18 }\n"
                  "struct S18 { f: S19 }\n"
                  "struct S19 { f: S20 }\n"
                  "struct S20 { f: S21 }\n"
                  "struct S21 { f: S22 }\n"
                  "struct S22 { f: S23 }\n"
                  "struct S23 { f: S24 }\n"
                  "struct S24 { f: S25 }\n"
                  "struct S25 { f: S26 }\n"
                  "struct S26 { f: S27 }\n"
                  "struct S27 { f: S28 }\n"
                  "struct S28 { f: S29 }\n"
                  "struct S29 { f: S30 }\n"
                  "struct S30 { f: S31 }\n"
                  "struct S31 { f: S32 }\n"
                  "struct S32 { f: S33 }\n"
                  "struct S33 { f: S34 }\n"
                  "struct S34 { f: S35 }\n"
                  "struct S35 { f: S36 }\n"
                  "struct S36 { f: S37 }\n"
                  "struct S37 { f: S38 }\n"
                  "struct S38 { f: S39 }\n"
                  "struct S39 { f: S0 }\n");

    assert(diagnostics_count(&ctx.diagnostics) == 1);

    const char *message = diagnostics_get(&ctx.diagnostics, 0)->message;

    assert(strstr(message, "contains ... 'S0'") != NULL);

    test_context_free(&ctx);
}

static void test_a_unit_may_declare_a_struct_called_array() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "struct Array { n: i32 }\n"
                  "struct Holder { a: Array, xs: array<i32, 2> }\n");

    assert(diagnostics_count(&ctx.diagnostics) == 0);

    test_context_free(&ctx);
}

static void test_rejects_type_arguments_on_a_primitive() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "struct V { a: i32<bool> }\n");

    assert(diagnostics_count(&ctx.diagnostics) == 1);
    assert(strcmp(diagnostics_get(&ctx.diagnostics, 0)->message, "i32 does not take a type argument") == 0);

    test_context_free(&ctx);
}

static void test_an_array_is_named_by_its_shape() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "func f(): i32 { let xs: array<i32, 3>; let y: i32 = xs; return y; }\n");

    assert(diagnostics_count(&ctx.diagnostics) == 1);
    assert(strcmp(diagnostics_get(&ctx.diagnostics, 0)->message,
                  "cannot initialize a variable of type i32 with a value of type array<i32, 3>") == 0);

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

    compile(&ctx, "func f(a: i32): i32 { return a; } func g(): i32 { return f(1, 2); }");

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

    compile(&ctx, "func f(a: i32): i32 { return a; } func g(): i32 { return f(1.5); }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "argument 1 is f32, but i32 was declared") == 0);

    test_context_free(&ctx);
}

static void test_reports_calling_a_non_function() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func g(): i32 { let x: i32 = 1; return x(); }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "this expression is not callable") == 0);

    test_context_free(&ctx);
}

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

static void test_syntax_errors_name_the_found_token() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: i32 = 1 }");

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

static void test_spans_track_lines_and_columns() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "module test;\nfunc test() {\n    let x: i32 = 1;\n    let y: i32 = nope;\n}");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(strcmp(diagnostic->message, "undeclared variable 'nope'") == 0);
    assert(diagnostic->span.line == 4);
    assert(diagnostic->span.column == 18);

    test_context_free(&ctx);
}

static void test_reports_borrowing_a_temporary() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let p: &i32 = 1; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot borrow a temporary; bind it to a variable first") == 0);

    test_context_free(&ctx);
}

static void test_reports_borrowing_a_call_result() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func g(): i32 { return 1; }\nfunc test() { let p: &i32 = g(); }");

    assert(diagnostics_count(diagnostics) == 1);
    assert(strcmp(diagnostics_get(diagnostics, 0)->message,
                  "cannot borrow a temporary; bind it to a variable first") == 0);

    test_context_free(&ctx);
}

static void test_reports_dereferencing_a_non_pointer() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: i32 = 1; let y: i32 = *x; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "cannot dereference i32") == 0);

    test_context_free(&ctx);
}

static void test_reports_field_access_through_a_non_struct_pointer() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test() { let x: i32 = 1; let p: &i32 = x; let y: i32 = p.field; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "&i32 is not a struct, so it has no fields") == 0);

    test_context_free(&ctx);
}

static void test_reports_returning_a_pointer_to_a_local() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "func test(): &i32 { let x: i32 = 1; return x; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_LIFETIME);
    assert(strcmp(diagnostic->message, "this borrow outlives what it names, so it cannot be returned") == 0);

    test_context_free(&ctx);
}

static void test_reports_a_pointer_escaping_its_block() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Node { n: i32 }\n"
                  "func test(): i32 {\n"
                  "    let p: &Node;\n"
                  "    { let x = Node { n: 1 }; p = x; }\n"
                  "    return p.n;\n"
                  "}\n");

    assert(diagnostics_count(diagnostics) > 0);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_LIFETIME);
    assert(strcmp(diagnostic->message, "this borrow outlives what it names, so it cannot be stored here") ==
           0);

    test_context_free(&ctx);
}

static void test_reports_a_stack_pointer_stored_into_a_heap_object() {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    compile(&ctx, "struct Inner { n: i32 }\n"
                  "struct Outer { child: &Inner }\n"
                  "func test() { let o: *Outer = box Outer { child: box Inner { n: 0 } }; let local = Inner "
                  "{ n: 0 }; o.child = local; }");

    assert(diagnostics_count(diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_LIFETIME);
    assert(strcmp(diagnostic->message, "this borrow outlives what it names, so it cannot be stored here") ==
           0);

    test_context_free(&ctx);
}

static void test_accepts_pointers_that_do_not_outlive_their_pointee() {
    TestContext ctx;
    test_context_init(&ctx);

    compile(&ctx, "func take(p: &i32): i32 { return *p; }\n"
                  "func test(): i32 {\n"
                  "let x: i32 = 1;\n"
                  "let p: &i32 = x;\n"
                  "{ let inner: &i32 = x; }\n"
                  "return take(x) + *p;\n"
                  "}");

    assert(diagnostics_count(&ctx.diagnostics) == 0);

    test_context_free(&ctx);
}

int main(void) {
    test_empty_sink_has_no_errors();
    test_a_struct_local_without_a_literal_reports_once();
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
    test_reports_mutual_containment_cycle();
    test_an_array_is_named_by_its_shape();
    test_a_unit_may_declare_a_struct_called_array();
    test_rejects_type_arguments_on_a_primitive();
    test_reports_the_path_a_containment_ring_takes();
    test_a_second_ring_traces_only_its_own_path();
    test_a_ring_reached_from_outside_names_only_the_ring();
    test_a_ring_too_long_to_name_is_elided_at_a_hop();
    test_reports_every_bad_field();

    test_reports_wrong_argument_count();
    test_reports_wrong_argument_type();
    test_reports_calling_a_non_function();

    test_reports_borrowing_a_temporary();
    test_reports_borrowing_a_call_result();
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
