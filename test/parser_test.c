#include <assert.h>
#include <parser.h>
#include <stdbool.h>
#include <stdio.h>

// --- Test Cases ---
static void test_single_number() {
    Lexer lexer = lexer_new("42");
    ASTNode *ast = parse(&lexer);
    assert(ast != NULL);
    assert(ast->type == NODE_NUMBER);
    assert(ast->number == 42.0);
    ast_free(ast);
}

static void test_simple_addition() {
    Lexer lexer = lexer_new("3 + 4");
    ASTNode *ast = parse(&lexer);
    assert(ast != NULL);
    assert(ast->type == NODE_BIN_OP);
    assert(ast->op == TOKEN_PLUS);
    assert(ast->left->type == NODE_NUMBER);
    assert(ast->left->number == 3.0);
    assert(ast->right->type == NODE_NUMBER);
    assert(ast->right->number == 4.0);
    ast_free(ast);
}

static void test_operator_precedence() {
    Lexer lexer = lexer_new("3 + 4 * 2");
    ASTNode *ast = parse(&lexer);
    assert(ast != NULL);

    // Expect: 3 + (4 * 2)
    assert(ast->type == NODE_BIN_OP);
    assert(ast->op == TOKEN_PLUS);
    assert(ast->left->type == NODE_NUMBER);
    assert(ast->left->number == 3.0);

    ASTNode *rhs = ast->right;
    assert(rhs->type == NODE_BIN_OP);
    assert(rhs->op == TOKEN_MUL);
    assert(rhs->left->type == NODE_NUMBER);
    assert(rhs->left->number == 4.0);
    assert(rhs->right->type == NODE_NUMBER);
    assert(rhs->right->number == 2.0);

    ast_free(ast);
}

static void test_parentheses() {
    Lexer lexer = lexer_new("(3 + 4) * 2");
    ASTNode *ast = parse(&lexer);
    assert(ast != NULL);

    printf("%d\n", ast->type);
    // Expect: (3 + 4) * 2
    assert(ast->type == NODE_BIN_OP);
    assert(ast->op == TOKEN_MUL);

    ASTNode *lhs = ast->left;
    assert(lhs->type == NODE_BIN_OP);
    assert(lhs->op == TOKEN_PLUS);
    assert(lhs->left->type == NODE_NUMBER);
    assert(lhs->left->number == 3.0);
    assert(lhs->right->type == NODE_NUMBER);
    assert(lhs->right->number == 4.0);

    assert(ast->right->type == NODE_NUMBER);
    assert(ast->right->number == 2.0);

    ast_free(ast);
}

static void test_invalid_token() {
    Lexer lexer = lexer_new("3 + $");
    ASTNode *ast = parse(&lexer);
    assert(ast == NULL); // Should fail
}

static void test_missing_paren() {
    Lexer lexer = lexer_new("(3 + 4");
    ASTNode *ast = parse(&lexer);
    assert(ast == NULL); // Should fail
}

// --- Main Test Runner ---
int main() {
    test_single_number();
    test_simple_addition();
    test_operator_precedence();
    test_parentheses();
    test_invalid_token();
    test_missing_paren();

    return 0;
}
