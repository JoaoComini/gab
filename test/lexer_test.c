#include <assert.h>
#include <lexer.h>
#include <string.h>

void assert_token(Lexer *lexer, TokenType expected_type) {
    Token token = lexer_next(lexer);
    assert(token.type == expected_type);
}

void assert_number(Lexer *lexer, double expected_value) {
    Token token = lexer_next(lexer);
    assert(token.type == TOKEN_NUMBER);
    assert(token.value.number == expected_value);
}

void assert_identifier(Lexer *lexer, char *expected_name) {
    Token token = lexer_next(lexer);
    assert(token.type == TOKEN_IDENT);
    assert(strcmp(token.value.identifier, expected_name) == 0);
}

void test_numbers() {
    Lexer lexer = lexer_new("42 3.14 .5 1e3");
    assert_number(&lexer, 42);
    assert_number(&lexer, 3.14);
    assert_number(&lexer, 0.5);  // .5 → 0.5
    assert_number(&lexer, 1000); // 1e3 → 1000
    assert_token(&lexer, TOKEN_EOF);
}

void test_operators() {
    Lexer lexer = lexer_new("+-*/");
    assert_token(&lexer, TOKEN_PLUS);
    assert_token(&lexer, TOKEN_MINUS);
    assert_token(&lexer, TOKEN_MUL);
    assert_token(&lexer, TOKEN_DIV);
    assert_token(&lexer, TOKEN_EOF);
}

void test_parentheses() {
    Lexer lexer = lexer_new("( )");
    assert_token(&lexer, TOKEN_LPAREN);
    assert_token(&lexer, TOKEN_RPAREN);
    assert_token(&lexer, TOKEN_EOF);
}

void test_whitespace() {
    Lexer lexer = lexer_new("  \t\n42 \n + ");
    assert_number(&lexer, 42);
    assert_token(&lexer, TOKEN_PLUS);
    assert_token(&lexer, TOKEN_EOF);
}

void test_identifiers() {
    Lexer lexer = lexer_new("variable1 variable2");
    assert_identifier(&lexer, "variable1");
    assert_identifier(&lexer, "variable2");
}

void test_errors() {
    Lexer lexer = lexer_new("42 $ +");
    assert_number(&lexer, 42);
    assert_token(&lexer, TOKEN_ERROR); // '$' is invalid
}

int main(void) {
    test_numbers();
    test_operators();
    test_parentheses();
    test_whitespace();
    test_identifiers();
    test_errors();
    return 0;
}
