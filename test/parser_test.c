#include "ast.h"
#include "lexer.h"
#include <assert.h>
#include <parser.h>
#include <stdbool.h>
#include <string.h>

// --- Test Cases ---
static void test_single_number() {
    Lexer lexer = lexer_create("42;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->count == 1);

    ASTStmt *stmt = script->statements[0];
    assert(stmt->type == STMT_EXPR);
    assert(stmt->expr.value->type == EXPR_LITERAL);
    assert(stmt->expr.value->literal.value.type == VARIANT_NUMBER);
    assert(stmt->expr.value->literal.value.number == 42.0);

    ast_script_free(script);
}

static void test_multiple_statements() {
    Lexer lexer = lexer_create("42; 3 + 5;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->count == 2);

    ASTStmt *first = script->statements[0];
    assert(first->type == STMT_EXPR);

    ASTStmt *second = script->statements[1];
    assert(second->type == STMT_EXPR);

    ast_script_free(script);
}

static void test_simple_addition() {
    Lexer lexer = lexer_create("3 + 4;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->count == 1);

    ASTStmt *stmt = script->statements[0];
    assert(stmt->type == STMT_EXPR);
    assert(stmt->expr.value->bin_op.op == BIN_OP_ADD);
    assert(stmt->expr.value->bin_op.left->type == EXPR_LITERAL);
    assert(stmt->expr.value->bin_op.left->literal.value.number == 3.0);
    assert(stmt->expr.value->bin_op.right->type == EXPR_LITERAL);
    assert(stmt->expr.value->bin_op.right->literal.value.number == 4.0);

    ast_script_free(script);
}

static void test_operator_precedence() {
    Lexer lexer = lexer_create("3 + 4 * 2;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->count == 1);

    ASTStmt *stmt = script->statements[0];
    assert(stmt->type == STMT_EXPR);

    ASTExpr *expr = stmt->expr.value;

    // Expect: 3 + (4 * 2)
    assert(expr->type == EXPR_BIN_OP);
    assert(expr->bin_op.op == BIN_OP_ADD);
    assert(expr->bin_op.left->type == EXPR_LITERAL);
    assert(expr->bin_op.left->literal.value.number == 3.0);

    ASTExpr *rhs = expr->bin_op.right;
    assert(rhs->type == EXPR_BIN_OP);
    assert(rhs->bin_op.op == BIN_OP_MUL);
    assert(rhs->bin_op.left->type == EXPR_LITERAL);
    assert(rhs->bin_op.left->literal.value.number == 4.0);
    assert(rhs->bin_op.right->type == EXPR_LITERAL);
    assert(rhs->bin_op.right->literal.value.number == 2.0);

    ast_expr_free(expr);
}

static void test_parentheses() {
    Lexer lexer = lexer_create("(3 + 4) * 2;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(parser.ok);
    assert(script != NULL);
    assert(script->count == 1);

    ASTStmt *stmt = script->statements[0];
    assert(stmt->type == STMT_EXPR);

    ASTExpr *expr = stmt->expr.value;

    // Expect: (3 + 4) * 2
    assert(expr->type == EXPR_BIN_OP);
    assert(expr->bin_op.op == BIN_OP_MUL);

    ASTExpr *lhs = expr->bin_op.left;
    assert(lhs->type == EXPR_BIN_OP);
    assert(lhs->bin_op.op == BIN_OP_ADD);
    assert(lhs->bin_op.left->type == EXPR_LITERAL);
    assert(lhs->bin_op.left->literal.value.number == 3.0);
    assert(lhs->bin_op.right->type == EXPR_LITERAL);
    assert(lhs->bin_op.right->literal.value.number == 4.0);

    assert(expr->bin_op.right->type == EXPR_LITERAL);
    assert(expr->bin_op.right->literal.value.number == 2.0);

    ast_script_free(script);
}

static void test_variables() {
    Lexer lexer = lexer_create("2 + (x * y);");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->count == 1);

    ASTStmt *stmt = script->statements[0];
    assert(stmt->type == STMT_EXPR);

    ASTExpr *expr = stmt->expr.value;

    assert(expr->type == EXPR_BIN_OP);
    assert(expr->bin_op.op == BIN_OP_ADD);

    ASTExpr *rhs = expr->bin_op.right;
    assert(rhs->type == EXPR_BIN_OP);
    assert(rhs->bin_op.op == BIN_OP_MUL);
    assert(rhs->bin_op.left->type == EXPR_VARIABLE);
    assert(strcmp(rhs->bin_op.left->variable.name, "x") == 0);
    assert(rhs->bin_op.right->type == EXPR_VARIABLE);
    assert(strcmp(rhs->bin_op.right->variable.name, "y") == 0);

    assert(expr->bin_op.left->type == EXPR_LITERAL);
    assert(expr->bin_op.left->literal.value.number == 2.0);

    ast_script_free(script);
}

static void test_declaration() {
    Lexer lexer = lexer_create("let x = 2 + 3;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->count == 1);

    ASTStmt *stmt = script->statements[0];
    assert(stmt->type == STMT_VAR_DECL);

    assert(strcmp(stmt->var_decl.name, "x") == 0);

    ASTExpr *initializer = stmt->var_decl.initializer;

    assert(initializer->type == EXPR_BIN_OP);
    assert(initializer->bin_op.op == BIN_OP_ADD);

    ASTExpr *lhs = initializer->bin_op.left;
    assert(lhs->type == EXPR_LITERAL);
    assert(lhs->literal.value.number == 2);

    ASTExpr *rhs = initializer->bin_op.right;
    assert(rhs->type == EXPR_LITERAL);
    assert(rhs->literal.value.number == 3);

    ast_script_free(script);
}

static void test_unintialized_declaration() {
    Lexer lexer = lexer_create("let x;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->count == 1);

    ASTStmt *stmt = script->statements[0];
    assert(stmt->type == STMT_VAR_DECL);

    assert(strcmp(stmt->var_decl.name, "x") == 0);
    assert(stmt->var_decl.initializer == NULL);

    ast_script_free(script);
}

static void test_assignment() {
    Lexer lexer = lexer_create("x = 2;");
    Parser parser = parser_create(&lexer);
    ASTScript *script = parser_parse(&parser);
    assert(script != NULL);

    assert(script->count == 1);

    ASTStmt *stmt = script->statements[0];
    assert(stmt->type == STMT_ASSIGN);

    ASTExpr *target = stmt->assign.target;
    assert(target->type == EXPR_VARIABLE);
    assert(strcmp(target->variable.name, "x") == 0);

    ASTExpr *value = stmt->assign.value;
    assert(value->type == EXPR_LITERAL);
    assert(value->literal.value.number == 2.0);

    ast_script_free(script);
}

static void test_invalid_token() {
    Lexer lexer = lexer_create("3 + $");
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

int main() {
    test_single_number();
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
    test_assignment();

    return 0;
}
