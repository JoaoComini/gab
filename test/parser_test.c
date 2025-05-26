#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "string/string.h"
#include "type.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

// --- Test Cases ---
static void test_single_number() {
    Lexer lexer = lexer_create("42;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
    assert(stmt->kind == STMT_EXPR);
    assert(stmt->expr.value->kind == EXPR_LITERAL);
    assert(stmt->expr.value->lit.kind == TYPE_INT);
    assert(stmt->expr.value->lit.as_int == 42);

    ast_script_destroy(script);
}

static void test_booleans() {
    Lexer lexer = lexer_create("true; false;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->statements.size == 2);

    ASTStmt *true_stmt = script->statements.data[0];
    assert(true_stmt->kind == STMT_EXPR);
    assert(true_stmt->expr.value->kind == EXPR_LITERAL);
    assert(true_stmt->expr.value->lit.kind == TYPE_BOOL);
    assert(true_stmt->expr.value->lit.as_int == 1);

    ASTStmt *false_stmt = script->statements.data[1];
    assert(false_stmt->kind == STMT_EXPR);
    assert(false_stmt->expr.value->kind == EXPR_LITERAL);
    assert(false_stmt->expr.value->lit.kind == TYPE_BOOL);
    assert(false_stmt->expr.value->lit.as_int == 0);

    ast_script_destroy(script);
}

static void test_multiple_statements() {
    Lexer lexer = lexer_create("42; 3 + 5;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->statements.size == 2);

    ASTStmt *first = script->statements.data[0];
    assert(first->kind == STMT_EXPR);

    ASTStmt *second = script->statements.data[1];
    assert(second->kind == STMT_EXPR);

    ast_script_destroy(script);
}

static void test_simple_addition() {
    Lexer lexer = lexer_create("3 + 4;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
    assert(stmt->kind == STMT_EXPR);
    assert(stmt->expr.value->bin_op.op == BIN_OP_ADD);
    assert(stmt->expr.value->bin_op.left->kind == EXPR_LITERAL);
    assert(stmt->expr.value->bin_op.left->lit.as_int == 3);
    assert(stmt->expr.value->bin_op.right->kind == EXPR_LITERAL);
    assert(stmt->expr.value->bin_op.right->lit.as_int == 4);

    ast_script_destroy(script);
}

static void test_operator_precedence() {
    Lexer lexer = lexer_create("3 + 4 * 2;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
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
    Lexer lexer = lexer_create("(3 + 4) * 2;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
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
    Lexer lexer = lexer_create("let x = 2; let y = 3; 2 + (x * y);");

    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->statements.size == 3);

    ASTStmt *stmt = script->statements.data[2];
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

    ast_script_destroy(script);
}

static void test_declaration() {
    Lexer lexer = lexer_create("let x = 2 + 3;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
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

static void test_unintialized_declaration() {
    Lexer lexer = lexer_create("let x: int;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
    assert(stmt->kind == STMT_VAR_DECL);

    assert(string_ref_equals_cstr(stmt->var_decl.name, "x"));
    assert(stmt->var_decl.initializer == NULL);

    ast_script_destroy(script);
}

static void test_untyped_and_unintialized_declaration() {
    Lexer lexer = lexer_create("let x;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);

    assert(!parser.ok);
    assert(strcmp(parser.error.message, "expected type after identifier") == 0);
    assert(script == NULL);

    ast_script_destroy(script);
}

static void test_assignment() {
    Lexer lexer = lexer_create("x = 2;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
    assert(stmt->kind == STMT_ASSIGN);

    ASTExpr *target = stmt->assign.target;
    assert(target->kind == EXPR_VARIABLE);
    assert(string_ref_equals_cstr(target->var.name, "x"));

    ASTExpr *value = stmt->assign.value;
    assert(value->kind == EXPR_LITERAL);
    assert(value->lit.as_int == 2.0);

    ast_script_destroy(script);
}

static void test_block() {
    Lexer lexer = lexer_create("{ let x = 2; x = 1; }");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
    assert(stmt->kind == STMT_BLOCK);
    assert(stmt->block.list.size == 2);

    ast_script_destroy(script);
}

static void test_if() {
    Lexer lexer = lexer_create("if 2 < 1 { 10; } else { 20; }");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
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
    Lexer lexer = lexer_create("return 2;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->statements.size == 1);

    ASTStmt *stmt = script->statements.data[0];
    assert(stmt->kind == STMT_RETURN);

    ASTExpr *result = stmt->ret.result;
    assert(result->kind == EXPR_LITERAL);
    assert(result->lit.as_float == 2.0);

    ast_script_destroy(script);
}
static void test_invalid_token() {
    Lexer lexer = lexer_create("3 + $;");
    Parser parser = parser_create(&lexer);
    ASTScript *ast = parser_parse(&parser);

    assert(!parser.ok);
    assert(strcmp(parser.error.message, "expected expression") == 0);
    assert(ast == NULL); // Should fail
}

static void test_missing_paren() {
    Lexer lexer = lexer_create("(3 + 4");
    Parser parser = parser_create(&lexer);
    ASTScript *ast = parser_parse(&parser);

    assert(!parser.ok);
    assert(strcmp(parser.error.message, "expected ')'") == 0);
    assert(ast == NULL); // Should fail
}

static void test_missing_identifier() {
    Lexer lexer = lexer_create("let ;");
    Parser parser = parser_create(&lexer);
    ASTScript *ast = parser_parse(&parser);

    assert(!parser.ok);
    assert(strcmp(parser.error.message, "expected identifier after 'let'") == 0);
    assert(ast == NULL); // Should fail
}

static void test_invalid_declaration() {
    Lexer lexer = lexer_create("let a b");
    Parser parser = parser_create(&lexer);
    ASTScript *ast = parser_parse(&parser);

    assert(!parser.ok);
    assert(strcmp(parser.error.message, "expected ';' or '='") == 0);
    assert(ast == NULL); // Should fail
}

static void test_missing_semicolon() {
    Lexer lexer = lexer_create("3 + 5");
    Parser parser = parser_create(&lexer);
    ASTScript *ast = parser_parse(&parser);

    assert(!parser.ok);
    assert(strcmp(parser.error.message, "expected ';' after statement") == 0);
    assert(ast == NULL); // Should fail
}

static void test_expression_not_assignable() {
    Lexer lexer = lexer_create("2 = 1");
    Parser parser = parser_create(&lexer);
    ASTScript *ast = parser_parse(&parser);

    assert(!parser.ok);
    assert(strcmp(parser.error.message, "expression is not assignable") == 0);
    assert(ast == NULL);
}

int main() {
    string_init();

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
    test_declaration();
    test_unintialized_declaration();
    test_untyped_and_unintialized_declaration();
    test_assignment();
    test_block();
    test_if();

    string_deinit();

    return 0;
}
