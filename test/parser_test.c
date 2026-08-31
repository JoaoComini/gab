#include "arena.h"
#include "ast/ast.h"
#include "ast/stmt.h"
#include "diagnostics.h"
#include "lexer.h"
#include "parser.h"
#include "support/test_context.h"
#include "type/type.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* A parsed unit points into the context's arena, so the context outlives every unit handed back. */
static TestContext parsed;

static ASTUnit *assert_parse(const char *code) {
    Diagnostics *diagnostics = &parsed.diagnostics;

    ASTUnit *unit = ast_unit_create(parsed.arena);
    Lexer lexer = lexer_create(test_in_a_module(code), parsed.arena, &parsed.strings, diagnostics);
    Parser parser = parser_create(&lexer, diagnostics);
    bool ok = parser_parse(&parser, unit);
    assert(ok);
    assert(!diagnostics_has_errors(diagnostics));

    return unit;
}

static void assert_parse_error(const char *code, const char *expected_error) {
    TestContext ctx;
    test_context_init(&ctx);
    Diagnostics *diagnostics = &ctx.diagnostics;

    ASTUnit *unit = ast_unit_create(ctx.arena);
    Lexer lexer = lexer_create(test_in_a_module(code), ctx.arena, &ctx.strings, diagnostics);
    Parser parser = parser_create(&lexer, diagnostics);
    bool ok = parser_parse(&parser, unit);
    assert(!ok);

    assert(diagnostics_has_errors(diagnostics));

    bool found = false;
    for (size_t i = 0; i < diagnostics_count(diagnostics); i++) {
        if (strcmp(diagnostics_get(diagnostics, i)->message, expected_error) == 0) {
            found = true;
            break;
        }
    }

    if (!found) {
        diagnostics_print(diagnostics, stderr);
    }

    assert(found);

    test_context_free(&ctx);
}

char code_buffer[100];
static const char *func_wrap(const char *code) {
    snprintf(code_buffer, sizeof(code_buffer), "func test() { %s }", code);

    return code_buffer;
}

static ASTStmtList func_unwrap(ASTUnit *unit) { return unit->statements.data[0]->func_decl.body->block.list; }

static void test_single_number() {
    ASTUnit *unit = assert_parse(func_wrap("42;"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_EXPR);
    assert(stmt->expr.value->kind == EXPR_LITERAL);
    assert(stmt->expr.value->lit.kind == TYPE_INT);
    assert(stmt->expr.value->lit.as_int == 42);
}

static void test_booleans() {
    ASTUnit *unit = assert_parse(func_wrap("true; false;"));

    ASTStmt *true_stmt = func_unwrap(unit).data[0];
    assert(true_stmt->kind == STMT_EXPR);
    assert(true_stmt->expr.value->kind == EXPR_LITERAL);
    assert(true_stmt->expr.value->lit.kind == TYPE_BOOL);
    assert(true_stmt->expr.value->lit.as_int == 1);

    ASTStmt *false_stmt = func_unwrap(unit).data[1];
    assert(false_stmt->kind == STMT_EXPR);
    assert(false_stmt->expr.value->kind == EXPR_LITERAL);
    assert(false_stmt->expr.value->lit.kind == TYPE_BOOL);
    assert(false_stmt->expr.value->lit.as_int == 0);
}

static void test_multiple_statements() {
    ASTUnit *unit = assert_parse(func_wrap("42; 3 + 5;"));

    ASTStmt *first = func_unwrap(unit).data[0];
    assert(first->kind == STMT_EXPR);

    ASTStmt *second = func_unwrap(unit).data[1];
    assert(second->kind == STMT_EXPR);
}

static void test_simple_addition() {
    ASTUnit *unit = assert_parse(func_wrap("3 + 4;"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_EXPR);
    assert(stmt->expr.value->bin_op.op == BIN_OP_ADD);
    assert(stmt->expr.value->bin_op.left->kind == EXPR_LITERAL);
    assert(stmt->expr.value->bin_op.left->lit.as_int == 3);
    assert(stmt->expr.value->bin_op.right->kind == EXPR_LITERAL);
    assert(stmt->expr.value->bin_op.right->lit.as_int == 4);
}

static void test_operator_precedence() {
    ASTUnit *unit = assert_parse(func_wrap("3 + 4 * 2;"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_EXPR);

    ASTExpr *expr = stmt->expr.value;

    assert(expr->kind == EXPR_BIN_OP);
    assert(expr->bin_op.op == BIN_OP_ADD);
    assert(expr->bin_op.left->kind == EXPR_LITERAL);
    assert(expr->bin_op.left->lit.as_int == 3.0);

    ASTExpr *rhs = expr->bin_op.right;
    assert(rhs->kind == EXPR_BIN_OP);
    assert(rhs->bin_op.op == BIN_OP_MUL);
    assert(rhs->bin_op.left->kind == EXPR_LITERAL);
    assert(rhs->bin_op.left->lit.as_int == 4.0);
    assert(rhs->bin_op.right->kind == EXPR_LITERAL);
    assert(rhs->bin_op.right->lit.as_int == 2.0);
}

static void test_parentheses() {
    ASTUnit *unit = assert_parse(func_wrap("(3 + 4) * 2;"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_EXPR);

    ASTExpr *expr = stmt->expr.value;

    assert(expr->kind == EXPR_BIN_OP);
    assert(expr->bin_op.op == BIN_OP_MUL);

    ASTExpr *lhs = expr->bin_op.left;
    assert(lhs->kind == EXPR_BIN_OP);
    assert(lhs->bin_op.op == BIN_OP_ADD);
    assert(lhs->bin_op.left->kind == EXPR_LITERAL);
    assert(lhs->bin_op.left->lit.as_int == 3.0);
    assert(lhs->bin_op.right->kind == EXPR_LITERAL);
    assert(lhs->bin_op.right->lit.as_int == 4.0);

    assert(expr->bin_op.right->kind == EXPR_LITERAL);
    assert(expr->bin_op.right->lit.as_int == 2.0);
}

static void test_variables() {
    ASTUnit *unit = assert_parse(func_wrap("let x = 2; let y = 3; 2 + (x * y);"));

    ASTStmt *stmt = func_unwrap(unit).data[2];
    assert(stmt->kind == STMT_EXPR);

    ASTExpr *expr = stmt->expr.value;

    assert(expr->kind == EXPR_BIN_OP);
    assert(expr->bin_op.op == BIN_OP_ADD);

    ASTExpr *rhs = expr->bin_op.right;
    assert(rhs->kind == EXPR_BIN_OP);
    assert(rhs->bin_op.op == BIN_OP_MUL);
    assert(rhs->bin_op.left->kind == EXPR_VARIABLE);
    assert(string_ref_equals_cstr(rhs->bin_op.left->var.name, "x"));
    assert(rhs->bin_op.right->kind == EXPR_VARIABLE);
    assert(string_ref_equals_cstr(rhs->bin_op.right->var.name, "y"));

    assert(expr->bin_op.left->kind == EXPR_LITERAL);
    assert(expr->bin_op.left->lit.as_int == 2.0);
}

static void test_var_declaration() {
    ASTUnit *unit = assert_parse(func_wrap("let x = 2 + 3;"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_VAR_DECL);

    assert(string_ref_equals_cstr(stmt->var_decl.name, "x"));

    ASTExpr *initializer = stmt->var_decl.initializer;

    assert(initializer->kind == EXPR_BIN_OP);
    assert(initializer->bin_op.op == BIN_OP_ADD);

    ASTExpr *lhs = initializer->bin_op.left;
    assert(lhs->kind == EXPR_LITERAL);
    assert(lhs->lit.as_int == 2);

    ASTExpr *rhs = initializer->bin_op.right;
    assert(rhs->kind == EXPR_LITERAL);
    assert(rhs->lit.as_int == 3);
}

static void test_var_uninit_declaration() {
    ASTUnit *unit = assert_parse(func_wrap("let x: int;"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_VAR_DECL);

    assert(string_ref_equals_cstr(stmt->var_decl.name, "x"));
    assert(stmt->var_decl.initializer == NULL);
}

static void test_var_untyped_uninti_declaration() {
    assert_parse_error(func_wrap("let x;"), "expected a type or an initializer");
}

static void test_struct_declaration() {
    ASTUnit *unit = assert_parse("struct Vec3 { x: float, y: float, z: float }");

    ASTStmt *stmt = unit->statements.data[0];
    assert(stmt->kind == STMT_STRUCT_DECL);
    assert(string_ref_equals_cstr(stmt->struct_decl.name, "Vec3"));

    ASTFieldList fields = stmt->struct_decl.fields;
    assert(fields.size == 3);

    assert(string_ref_equals_cstr(fields.data[0]->name, "x"));
    assert(string_ref_equals_cstr(fields.data[0]->type_expr->name, "float"));
    assert(string_ref_equals_cstr(fields.data[1]->name, "y"));
    assert(string_ref_equals_cstr(fields.data[2]->name, "z"));
}

static void test_struct_trailing_comma() {
    ASTUnit *unit = assert_parse("struct Pair { a: int, b: int, }");

    ASTStmt *stmt = unit->statements.data[0];
    assert(stmt->kind == STMT_STRUCT_DECL);
    assert(stmt->struct_decl.fields.size == 2);
}

static void test_empty_struct_declaration() {
    ASTUnit *unit = assert_parse("struct Empty { }");

    ASTStmt *stmt = unit->statements.data[0];
    assert(stmt->kind == STMT_STRUCT_DECL);
    assert(stmt->struct_decl.fields.size == 0);
}

static void test_struct_missing_name() {
    assert_parse_error("struct { x: int }", "expected a struct name, found '{'");
}

static void test_struct_missing_colon() {
    assert_parse_error("struct Bad { x int }", "expected ':' after name, found an identifier");
}

static void test_struct_unterminated() {
    assert_parse_error("struct Bad { x: int", "expected ',' or '}' after field, found end of input");
}

static void test_func_declaration() {
    ASTUnit *unit = assert_parse("func add(x : int, y : int): int {"
                                 "    return x + y;"
                                 "}");

    ASTStmt *stmt = unit->statements.data[0];
    assert(stmt->kind == STMT_FUNC_DECL);

    assert(string_ref_equals_cstr(stmt->func_decl.name, "add"));
    assert(string_ref_equals_cstr(stmt->func_decl.return_type->name, "int"));

    ASTFieldList params = stmt->func_decl.params;
    assert(string_ref_equals_cstr(params.data[0]->name, "x"));
    assert(string_ref_equals_cstr(params.data[1]->name, "y"));

    ASTStmt *body = stmt->func_decl.body;
    assert(body->kind == STMT_BLOCK);
    assert(body->block.list.data[0]->kind == STMT_RETURN);
}

static void test_unit_func_declaration() {
    ASTUnit *unit = assert_parse("func test(x : int, y : int) {"
                                 "    let a = x + y;"
                                 "}");

    ASTStmt *stmt = unit->statements.data[0];
    assert(stmt->kind == STMT_FUNC_DECL);

    assert(string_ref_equals_cstr(stmt->func_decl.name, "test"));
    assert(stmt->func_decl.return_type == NULL);

    ASTFieldList params = stmt->func_decl.params;
    assert(string_ref_equals_cstr(params.data[0]->name, "x"));
    assert(string_ref_equals_cstr(params.data[1]->name, "y"));

    ASTStmt *body = stmt->func_decl.body;
    assert(body->kind == STMT_BLOCK);
    assert(body->block.list.data[0]->kind == STMT_VAR_DECL);
}

static void test_no_params_func_declaration() {
    ASTUnit *unit = assert_parse("func test() {"
                                 "    return true;"
                                 "}");

    ASTStmt *stmt = unit->statements.data[0];
    assert(stmt->kind == STMT_FUNC_DECL);
    assert(string_ref_equals_cstr(stmt->func_decl.name, "test"));
    assert(stmt->func_decl.return_type == NULL);

    ASTFieldList params = stmt->func_decl.params;
    assert(params.size == 0);

    ASTStmt *body = stmt->func_decl.body;
    assert(body->kind == STMT_BLOCK);
    assert(body->block.list.data[0]->kind == STMT_RETURN);
}
static void test_assignment() {
    ASTUnit *unit = assert_parse(func_wrap("x = 2;"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_ASSIGN);

    ASTExpr *target = stmt->assign.target;
    assert(target->kind == EXPR_VARIABLE);
    assert(string_ref_equals_cstr(target->var.name, "x"));

    ASTExpr *value = stmt->assign.value;
    assert(value->kind == EXPR_LITERAL);
    assert(value->lit.as_int == 2.0);
}

static void test_block() {
    ASTUnit *unit = assert_parse(func_wrap("{ let x = 2; x = 1; }"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_BLOCK);
    assert(stmt->block.list.size == 2);
}

static void test_if() {
    ASTUnit *unit = assert_parse(func_wrap("if 2 < 1 { 10; } else { 20; }"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_IF);

    ASTExpr *condition = stmt->ifstmt.condition;
    assert(condition->kind == EXPR_BIN_OP);

    ASTStmt *then_block = stmt->ifstmt.then_block;
    assert(then_block->kind == STMT_BLOCK);
    assert(then_block->block.list.data[0]->kind == STMT_EXPR);

    ASTStmt *else_block = stmt->ifstmt.else_block;
    assert(else_block->kind == STMT_BLOCK);
    assert(else_block->block.list.data[0]->kind == STMT_EXPR);
}

static void test_return() {
    ASTUnit *unit = assert_parse(func_wrap("return 2;"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_RETURN);

    ASTExpr *result = stmt->ret.result;
    assert(result->kind == EXPR_LITERAL);
    assert(result->lit.kind == TYPE_INT);
    assert(result->lit.as_int == 2);
}

static void test_invalid_token() {
    assert_parse_error(func_wrap("3 + $;"), "expected an expression, found invalid token");
}

static void test_missing_paren() { assert_parse_error(func_wrap("(3 + 4"), "expected ')', found '}'"); }

static void test_missing_identifier() {
    assert_parse_error(func_wrap("let ;"), "expected a variable name after 'let', found ';'");
}

static void test_invalid_declaration() {
    assert_parse_error(func_wrap("let a b"), "expected ';' or '=', found an identifier");
}

static void test_missing_semicolon() { assert_parse_error(func_wrap("3 + 5"), "expected ';', found '}'"); }

static void test_expression_not_assignable() {
    assert_parse_error(func_wrap("2 = 1"), "expression is not assignable");
}

static void test_module_directive() {
    ASTUnit *unit = assert_parse("module Player;\nfunc f(): int { return 1; }\n");

    assert(unit->module_name.data);
    assert(unit->module_name.length == 6);
    assert(strncmp(unit->module_name.data, "Player", 6) == 0);
    assert(unit->module_span.line == 1);

    assert(unit->statements.size == 1);
}

static void test_a_unit_must_name_its_module() {
    TestContext ctx;
    test_context_init(&ctx);

    ASTUnit *unit = ast_unit_create(ctx.arena);
    Lexer lexer = lexer_create("func f(): int { return 1; }\n", ctx.arena, &ctx.strings, &ctx.diagnostics);
    Parser parser = parser_create(&lexer, &ctx.diagnostics);

    assert(!parser_parse(&parser, unit));
    assert(diagnostics_has_errors(&ctx.diagnostics));

    test_context_free(&ctx);
}

static void test_module_directive_alone() {
    ASTUnit *unit = assert_parse("module Player;\n");

    assert(unit->module_name.data);
    assert(unit->statements.size == 0);
}

static void test_module_name_cannot_be_nested() {
    assert_parse_error("module Player::Movement;\nfunc f(): int { return 1; }\n",
                       "module names cannot be nested; 'Player::Movement' must be a single identifier");
}

static void test_module_must_come_first() {
    assert_parse_error("func f(): int { return 1; }\nmodule Player;\n",
                       "'module' must appear once, before any declaration");
}

static void test_module_cannot_be_declared_twice() {
    assert_parse_error("module A;\nmodule B;\nfunc f(): int { return 1; }\n",
                       "'module' must appear once, before any declaration");
}

static void test_module_needs_a_name() {
    assert_parse_error("module ;\n", "expected a module name after 'module', found ';'");
}

static void test_module_needs_a_semicolon() {
    assert_parse_error("module Player\nfunc f(): int { return 1; }\n",
                       "expected ';' after the module name, found 'func'");
}

static void test_function_cannot_be_declared_inside_another() {
    assert_parse_error("func outer(): int {\n"
                       "    func inner(): int { return 1; }\n"
                       "    return inner();\n"
                       "}\n",
                       "a function cannot be declared inside another; declare it at module level");
}

static void test_function_cannot_be_declared_inside_a_method() {
    assert_parse_error("struct P { n: int }\n"
                       "func P::m(p: &P): int {\n"
                       "    func inner(): int { return 1; }\n"
                       "    return 0;\n"
                       "}\n",
                       "a function cannot be declared inside another; declare it at module level");
}

static void test_for_infinite() {
    ASTUnit *unit = assert_parse(func_wrap("for { 10; }"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_FOR);

    assert(!stmt->forstmt.init);
    assert(!stmt->forstmt.condition);
    assert(!stmt->forstmt.post);
    assert(stmt->forstmt.body->kind == STMT_BLOCK);
}

static void test_for_condition() {
    ASTUnit *unit = assert_parse(func_wrap("for 2 < 1 { 10; }"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_FOR);

    assert(!stmt->forstmt.init);
    assert(stmt->forstmt.condition->kind == EXPR_BIN_OP);
    assert(!stmt->forstmt.post);
}

static void test_for_clauses() {
    ASTUnit *unit = assert_parse(func_wrap("for let i: int = 0; i < 3; i = i + 1 { 10; }"));

    ASTStmt *stmt = func_unwrap(unit).data[0];
    assert(stmt->kind == STMT_FOR);

    assert(stmt->forstmt.init->kind == STMT_VAR_DECL);
    assert(stmt->forstmt.condition->kind == EXPR_BIN_OP);
    assert(stmt->forstmt.post->kind == STMT_ASSIGN);

    unit = assert_parse(func_wrap("for ; ; { 10; }"));
    stmt = func_unwrap(unit).data[0];

    assert(!stmt->forstmt.init);
    assert(!stmt->forstmt.condition);
    assert(!stmt->forstmt.post);
}

static void test_break_and_continue() {
    ASTUnit *unit = assert_parse(func_wrap("for { break; continue; }"));

    ASTStmtList body = func_unwrap(unit).data[0]->forstmt.body->block.list;

    assert(body.data[0]->kind == STMT_JUMP);
    assert(body.data[0]->jump.is_break);

    assert(body.data[1]->kind == STMT_JUMP);
    assert(!body.data[1]->jump.is_break);
}

static void test_a_type_takes_several_arguments() {
    ASTUnit *unit = assert_parse("struct Holder { a: Map<int,float> }");

    TypeExpr *apply = unit->statements.data[0]->struct_decl.fields.data[0]->type_expr;

    assert(apply->kind == TYPE_EXPR_APPLY);
    assert(string_ref_equals_cstr(apply->apply.base->name, "Map"));
    assert(apply->apply.args.size == 2);
    assert(string_ref_equals_cstr(apply->apply.args.data[0]->name, "int"));
    assert(string_ref_equals_cstr(apply->apply.args.data[1]->name, "float"));
}

static void test_an_argument_may_be_an_application() {
    ASTUnit *unit = assert_parse("struct Holder { a: Vec<Vec<int>> }");

    TypeExpr *outer = unit->statements.data[0]->struct_decl.fields.data[0]->type_expr;

    assert(outer->kind == TYPE_EXPR_APPLY);
    assert(outer->apply.args.size == 1);

    TypeExpr *inner = outer->apply.args.data[0];

    assert(inner->kind == TYPE_EXPR_APPLY);
    assert(string_ref_equals_cstr(inner->apply.base->name, "Vec"));
    assert(string_ref_equals_cstr(inner->apply.args.data[0]->name, "int"));
}

static void test_a_type_is_a_tree() {
    ASTUnit *unit = assert_parse("struct Holder { a: &*[int; 3] }");

    ASTStmt *stmt = unit->statements.data[0];
    ASTFieldList fields = stmt->struct_decl.fields;

    TypeExpr *ref = fields.data[0]->type_expr;
    assert(ref->kind == TYPE_EXPR_REF);

    TypeExpr *box = ref->indirect.inner;
    assert(box->kind == TYPE_EXPR_BOX);

    TypeExpr *array = box->indirect.inner;
    assert(array->kind == TYPE_EXPR_ARRAY);
    assert(array->array.element->kind == TYPE_EXPR_NAME);
    assert(string_ref_equals_cstr(array->array.element->name, "int"));
    assert(array->array.length == 3);
}

int main() {
    test_context_init(&parsed);

    test_function_cannot_be_declared_inside_another();
    test_function_cannot_be_declared_inside_a_method();
    test_module_directive();
    test_a_unit_must_name_its_module();
    test_module_directive_alone();
    test_module_name_cannot_be_nested();
    test_module_must_come_first();
    test_module_cannot_be_declared_twice();
    test_module_needs_a_name();
    test_module_needs_a_semicolon();

    test_single_number();
    test_booleans();
    test_multiple_statements();
    test_simple_addition();
    test_operator_precedence();
    test_parentheses();
    test_invalid_token();
    test_missing_paren();
    test_missing_identifier();
    test_invalid_declaration();
    test_missing_semicolon();
    test_expression_not_assignable();

    test_struct_declaration();
    test_struct_trailing_comma();
    test_empty_struct_declaration();
    test_struct_missing_name();
    test_struct_missing_colon();
    test_struct_unterminated();
    test_variables();
    test_var_declaration();
    test_var_uninit_declaration();
    test_var_untyped_uninti_declaration();
    test_func_declaration();
    test_unit_func_declaration();
    test_no_params_func_declaration();
    test_assignment();
    test_block();
    test_if();
    test_for_infinite();
    test_for_condition();
    test_for_clauses();
    test_break_and_continue();
    test_return();
    test_a_type_is_a_tree();
    test_a_type_takes_several_arguments();
    test_an_argument_may_be_an_application();

    test_context_free(&parsed);

    return 0;
}
