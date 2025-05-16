#include <assert.h>
#include <lexer.h>
#include <string.h>

static void assert_token(Lexer *lexer, TokenType expected_type) {
    Token token = lexer_next(lexer);
    assert(token.type == expected_type);
}

static void assert_number(Lexer *lexer, double expected_value) {
    Token token = lexer_next(lexer);
    assert(token.type == TOKEN_NUMBER);
    assert(token.value.number == expected_value);
}

static void assert_identifier(Lexer *lexer, char *expected_name) {
    Token token = lexer_next(lexer);
    assert(token.type == TOKEN_IDENT);
    assert(strcmp(token.value.identifier, expected_name) == 0);

    token_free(&token);
}

static void test_numbers() {
    Lexer lexer = lexer_create("42 3.14 .5 1e3");
    assert_number(&lexer, 42);
    assert_number(&lexer, 3.14);
    assert_number(&lexer, 0.5);  // .5 → 0.5
    assert_number(&lexer, 1000); // 1e3 → 1000
    assert_token(&lexer, TOKEN_EOF);
}

static void test_operators() {
    Lexer lexer = lexer_create("+-*/=");
    assert_token(&lexer, TOKEN_PLUS);
    assert_token(&lexer, TOKEN_MINUS);
    assert_token(&lexer, TOKEN_MUL);
    assert_token(&lexer, TOKEN_DIV);
    assert_token(&lexer, TOKEN_EQUAL);
    assert_token(&lexer, TOKEN_EOF);
}

static void test_parentheses() {
    Lexer lexer = lexer_create("( )");
    assert_token(&lexer, TOKEN_LPAREN);
    assert_token(&lexer, TOKEN_RPAREN);
    assert_token(&lexer, TOKEN_EOF);
}

static void test_whitespace() {
    Lexer lexer = lexer_create("  \t\n42 \n + ");
    assert_number(&lexer, 42);
    assert_token(&lexer, TOKEN_PLUS);
    assert_token(&lexer, TOKEN_EOF);
}

static void test_identifiers() {
    Lexer lexer = lexer_create("variable1 variable2");
    assert_identifier(&lexer, "variable1");
    assert_identifier(&lexer, "variable2");
    assert_token(&lexer, TOKEN_EOF);
}

static void test_keywords() {
    Lexer lexer = lexer_create("let return");
    assert_token(&lexer, TOKEN_LET);
    assert_token(&lexer, TOKEN_RETURN);
}

static void test_errors() {
    Lexer lexer = lexer_create("42 $ +");
    assert_number(&lexer, 42);
    assert_token(&lexer, TOKEN_INVALID); // '$' is invalid
}

int main(void) {
    test_numbers();
    test_operators();
    test_parentheses();
    test_whitespace();
    test_identifiers();
    test_keywords();
    test_errors();
    return 0;
}
