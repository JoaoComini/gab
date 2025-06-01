#include "arena.h"
#include "ast/ast.h"
#include "ast/expr.h"
#include "ast/stmt.h"
#include "lexer.h"
#include "parser.h"
#include "type.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static Arena *arena = NULL;

static ASTScript *assert_parse(const char *code) {
    ASTScript *script = ast_script_create();
    Lexer lexer = lexer_create(code);
    Parser parser = parser_create(arena, &lexer);
    bool ok = parser_parse(&parser, script);
    assert(ok);

    return script;
}

static void assert_parse_error(const char *code, const char *expected_error) {
    ASTScript *script = ast_script_create();
    Lexer lexer = lexer_create(code);
    Parser parser = parser_create(arena, &lexer);
    bool ok = parser_parse(&parser, script);
    assert(!ok);

    assert(strcmp(parser.error.message, expected_error) == 0);

    ast_script_destroy(script);
}

char code_buffer[100];
static const char *func_wrap(const char *code) {
    snprintf(code_buffer, sizeof(code_buffer), "func test() { %s }", code);

    return code_buffer;
}

static ASTStmtList func_unwrap(ASTScript *script) {
    return script->statements.data[0]->func_decl.body->block.list;
}

// --- Test Cases ---
static void test_single_number() {
    ASTScript *script = assert_parse(func_wrap("42;"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_EXPR);
    assert(stmt->expr.value->kind == EXPR_LITERAL);
    assert(stmt->expr.value->lit.kind == TYPE_INT);
    assert(stmt->expr.value->lit.as_int == 42);

    ast_script_destroy(script);
}

static void test_booleans() {
    ASTScript *script = assert_parse(func_wrap("true; false;"));

    ASTStmt *true_stmt = func_unwrap(script).data[0];
    assert(true_stmt->kind == STMT_EXPR);
    assert(true_stmt->expr.value->kind == EXPR_LITERAL);
    assert(true_stmt->expr.value->lit.kind == TYPE_BOOL);
    assert(true_stmt->expr.value->lit.as_int == 1);

    ASTStmt *false_stmt = func_unwrap(script).data[1];
    assert(false_stmt->kind == STMT_EXPR);
    assert(false_stmt->expr.value->kind == EXPR_LITERAL);
    assert(false_stmt->expr.value->lit.kind == TYPE_BOOL);
    assert(false_stmt->expr.value->lit.as_int == 0);

    ast_script_destroy(script);
}

static void test_multiple_statements() {
    ASTScript *script = assert_parse(func_wrap("42; 3 + 5;"));

    ASTStmt *first = func_unwrap(script).data[0];
    assert(first->kind == STMT_EXPR);

    ASTStmt *second = func_unwrap(script).data[1];
    assert(second->kind == STMT_EXPR);

    ast_script_destroy(script);
}

static void test_simple_addition() {
    ASTScript *script = assert_parse(func_wrap("3 + 4;"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_EXPR);
    assert(stmt->expr.value->bin_op.op == BIN_OP_ADD);
    assert(stmt->expr.value->bin_op.left->kind == EXPR_LITERAL);
    assert(stmt->expr.value->bin_op.left->lit.as_int == 3);
    assert(stmt->expr.value->bin_op.right->kind == EXPR_LITERAL);
    assert(stmt->expr.value->bin_op.right->lit.as_int == 4);

    ast_script_destroy(script);
}

static void test_operator_precedence() {
    ASTScript *script = assert_parse(func_wrap("3 + 4 * 2;"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_EXPR);

    ASTExpr *expr = stmt->expr.value;

    // Expect: 3 + (4 * 2)
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

    ast_script_destroy(script);
}

static void test_parentheses() {
    ASTScript *script = assert_parse(func_wrap("(3 + 4) * 2;"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_EXPR);

    ASTExpr *expr = stmt->expr.value;

    // Expect: (3 + 4) * 2
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

    ast_script_destroy(script);
}

static void test_variables() {
    ASTScript *script = assert_parse(func_wrap("let x = 2; let y = 3; 2 + (x * y);"));

    ASTStmt *stmt = func_unwrap(script).data[2];
    assert(stmt->kind == STMT_EXPR);

    ASTExpr *expr = stmt->expr.value;

    assert(expr->kind == EXPR_BIN_OP);
    assert(expr->bin_op.op == BIN_OP_ADD);

    ASTExpr *rhs = expr->bin_op.right;
    assert(rhs->kind == EXPR_BIN_OP);
    assert(rhs->bin_op.op == BIN_OP_MUL);
    assert(rhs->bin_op.left->kind == EXPR_IDENTIFIER);
    assert(string_ref_equals_cstr(rhs->bin_op.left->ident.name, "x"));
    assert(rhs->bin_op.right->kind == EXPR_IDENTIFIER);
    assert(string_ref_equals_cstr(rhs->bin_op.right->ident.name, "y"));

    assert(expr->bin_op.left->kind == EXPR_LITERAL);
    assert(expr->bin_op.left->lit.as_int == 2.0);

    ast_script_destroy(script);
}

static void test_var_declaration() {
    ASTScript *script = assert_parse(func_wrap("let x = 2 + 3;"));

    ASTStmt *stmt = func_unwrap(script).data[0];
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

    ast_script_destroy(script);
}

static void test_var_uninit_declaration() {
    ASTScript *script = assert_parse(func_wrap("let x: int;"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_VAR_DECL);

    assert(string_ref_equals_cstr(stmt->var_decl.name, "x"));
    assert(stmt->var_decl.initializer == NULL);

    ast_script_destroy(script);
}

static void test_var_untyped_uninti_declaration() {
    assert_parse_error(func_wrap("let x;"), "expected type after identifier");
}

static void test_func_declaration() {
    ASTScript *script = assert_parse("func add(x : int, y : int): int {"
                                     "    return x + y;"
                                     "}");

    ASTStmt *stmt = script->statements.data[0];
    assert(stmt->kind == STMT_FUNC_DECL);

    assert(string_ref_equals_cstr(stmt->func_decl.name, "add"));
    assert(string_ref_equals_cstr(stmt->func_decl.return_type->name, "int"));

    ASTFieldList params = stmt->func_decl.params;
    assert(string_ref_equals_cstr(params.data[0]->name, "x"));
    assert(string_ref_equals_cstr(params.data[1]->name, "y"));

    ASTStmt *body = stmt->func_decl.body;
    assert(body->kind == STMT_BLOCK);
    assert(body->block.list.data[0]->kind == STMT_RETURN);

    ast_script_destroy(script);
}

static void test_unit_func_declaration() {
    ASTScript *script = assert_parse("func test(x : int, y : int) {"
                                     "    let a = x + y;"
                                     "}");

    ASTStmt *stmt = script->statements.data[0];
    assert(stmt->kind == STMT_FUNC_DECL);

    assert(string_ref_equals_cstr(stmt->func_decl.name, "test"));
    assert(stmt->func_decl.return_type == NULL);

    ASTFieldList params = stmt->func_decl.params;
    assert(string_ref_equals_cstr(params.data[0]->name, "x"));
    assert(string_ref_equals_cstr(params.data[1]->name, "y"));

    ASTStmt *body = stmt->func_decl.body;
    assert(body->kind == STMT_BLOCK);
    assert(body->block.list.data[0]->kind == STMT_VAR_DECL);

    ast_script_destroy(script);
}

static void test_no_params_func_declaration() {
    ASTScript *script = assert_parse("func test() {"
                                     "    return true;"
                                     "}");

    ASTStmt *stmt = script->statements.data[0];
    assert(stmt->kind == STMT_FUNC_DECL);
    assert(string_ref_equals_cstr(stmt->func_decl.name, "test"));
    assert(string_ref_equals_cstr(stmt->func_decl.return_type->name, "bool"));

    ASTFieldList params = stmt->func_decl.params;
    assert(params.size == 0);

    ASTStmt *body = stmt->func_decl.body;
    assert(body->kind == STMT_BLOCK);
    assert(body->block.list.data[0]->kind == STMT_RETURN);

    ast_script_destroy(script);
}
static void test_assignment() {
    ASTScript *script = assert_parse(func_wrap("x = 2;"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_ASSIGN);

    ASTExpr *target = stmt->assign.target;
    assert(target->kind == EXPR_IDENTIFIER);
    assert(string_ref_equals_cstr(target->ident.name, "x"));

    ASTExpr *value = stmt->assign.value;
    assert(value->kind == EXPR_LITERAL);
    assert(value->lit.as_int == 2.0);

    ast_script_destroy(script);
}

static void test_block() {
    ASTScript *script = assert_parse(func_wrap("{ let x = 2; x = 1; }"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_BLOCK);
    assert(stmt->block.list.size == 2);

    ast_script_destroy(script);
}

static void test_if() {
    ASTScript *script = assert_parse(func_wrap("if 2 < 1 { 10; } else { 20; }"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_IF);

    ASTExpr *condition = stmt->ifstmt.condition;
    assert(condition->kind == EXPR_BIN_OP);

    ASTStmt *then_block = stmt->ifstmt.then_block;
    assert(then_block->kind == STMT_BLOCK);
    assert(then_block->block.list.data[0]->kind == STMT_EXPR);

    ASTStmt *else_block = stmt->ifstmt.else_block;
    assert(else_block->kind == STMT_BLOCK);
    assert(else_block->block.list.data[0]->kind == STMT_EXPR);

    ast_script_destroy(script);
}

static void test_return() {
    ASTScript *script = assert_parse(func_wrap("return 2.0;"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_RETURN);

    ASTExpr *result = stmt->ret.result;
    assert(result->kind == EXPR_LITERAL);
    assert(result->lit.as_float == 2.0);

    ast_script_destroy(script);
}

static void test_empty_call_expr() {
    ASTScript *script = assert_parse(func_wrap("call();"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_EXPR);

    ASTExpr *call = stmt->expr.value;
    assert(call->kind == EXPR_CALL);
    assert(call->call.target->kind == EXPR_IDENTIFIER);
    assert(call->call.params.size == 0);

    ast_script_destroy(script);
}

static void test_call_args_expr() {
    ASTScript *script = assert_parse(func_wrap("call(a, b);"));

    ASTStmt *stmt = func_unwrap(script).data[0];
    assert(stmt->kind == STMT_EXPR);

    ASTExpr *call = stmt->expr.value;
    assert(call->kind == EXPR_CALL);
    assert(call->call.target->kind == EXPR_IDENTIFIER);
    assert(call->call.params.size == 2);

    ast_script_destroy(script);
}

static void test_invalid_token() { assert_parse_error(func_wrap("3 + $;"), "expected expression"); }

static void test_missing_paren() { assert_parse_error(func_wrap("(3 + 4"), "expected ')'"); }

static void test_missing_identifier() {
    assert_parse_error(func_wrap("let ;"), "expected identifier after 'let'");
}

static void test_invalid_declaration() { assert_parse_error(func_wrap("let a b"), "expected ';' or '='"); }

static void test_missing_semicolon() { assert_parse_error(func_wrap("3 + 5"), "expected ';'"); }

static void test_expression_not_assignable() {
    assert_parse_error(func_wrap("2 = 1"), "expression is not assignable");
}

int main() {
    string_init();

    arena = arena_create(1024);

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
    test_variables();
    test_var_declaration();
    test_var_uninit_declaration();
    test_var_untyped_uninti_declaration();
    test_func_declaration();
    test_unit_func_declaration();
    test_assignment();
    test_block();
    test_if();
    test_return();
    test_empty_call_expr();
    test_call_args_expr();

    arena_destroy(arena);

    string_deinit();

    return 0;
}
