#include <arena.h>
#include <assert.h>
#include <diagnostics.h>
#include <lexer.h>
#include <string.h>

#define TEST_ARENA_BLOCK_SIZE 2048

static Arena *arena = NULL;
static Diagnostics diagnostics;

// Every case lexes into a fresh sink, so a test can assert on exactly what its
// own source produced.
static Lexer test_lexer(const char *source) {
    diagnostics_free(&diagnostics);
    diagnostics_init(&diagnostics, arena, "<test>");

    return lexer_create(source, &diagnostics);
}

static void assert_token(Lexer *lexer, TokenType expected_type) {
    Token token = lexer_next(lexer);
    assert(token.type == expected_type);
}

static void assert_identifier(Lexer *lexer, char *expected_name) {
    Token token = lexer_next(lexer);
    assert(token.type == TOKEN_IDENT);
    assert(strlen(expected_name) == token.lexeme.length &&
           strncmp(token.lexeme.data, expected_name, token.lexeme.length) == 0);
}

static void test_numbers() {
    Lexer lexer = test_lexer("42 3.14 .5");
    assert_token(&lexer, TOKEN_INT);
    assert_token(&lexer, TOKEN_FLOAT);
    assert_token(&lexer, TOKEN_FLOAT);
    assert_token(&lexer, TOKEN_EOF);
}

// A '.' starts a float only when a digit follows it, so field access and the
// leading-dot literal can coexist.
static void test_dot_is_field_access_unless_a_digit_follows() {
    Lexer lexer = test_lexer("v.x .5 v.5");
    assert_identifier(&lexer, "v");
    assert_token(&lexer, TOKEN_DOT);
    assert_identifier(&lexer, "x");

    assert_token(&lexer, TOKEN_FLOAT);

    assert_identifier(&lexer, "v");
    assert_token(&lexer, TOKEN_FLOAT);
    assert_token(&lexer, TOKEN_EOF);
}

static void test_operators() {
    Lexer lexer = test_lexer("+ - * / = ! < > == != <= >= && ||");
    assert_token(&lexer, TOKEN_PLUS);
    assert_token(&lexer, TOKEN_MINUS);
    assert_token(&lexer, TOKEN_MUL);
    assert_token(&lexer, TOKEN_DIV);
    assert_token(&lexer, TOKEN_ASSIGN);
    assert_token(&lexer, TOKEN_NOT);
    assert_token(&lexer, TOKEN_LESS);
    assert_token(&lexer, TOKEN_GREATER);
    assert_token(&lexer, TOKEN_EQUAL);
    assert_token(&lexer, TOKEN_NEQUAL);
    assert_token(&lexer, TOKEN_LEQUAL);
    assert_token(&lexer, TOKEN_GEQUAL);
    assert_token(&lexer, TOKEN_AND);
    assert_token(&lexer, TOKEN_OR);
    assert_token(&lexer, TOKEN_EOF);
}

static void test_parentheses() {
    Lexer lexer = test_lexer("( )");
    assert_token(&lexer, TOKEN_LPAREN);
    assert_token(&lexer, TOKEN_RPAREN);
    assert_token(&lexer, TOKEN_EOF);
}

static void test_braces() {
    Lexer lexer = test_lexer("{ }");
    assert_token(&lexer, TOKEN_LBRACE);
    assert_token(&lexer, TOKEN_RBRACE);
    assert_token(&lexer, TOKEN_EOF);
}

static void test_colons() {
    Lexer lexer = test_lexer("; : ,");
    assert_token(&lexer, TOKEN_SEMICOLON);
    assert_token(&lexer, TOKEN_COLON);
    assert_token(&lexer, TOKEN_COMMA);
    assert_token(&lexer, TOKEN_EOF);
}

static void test_whitespace() {
    Lexer lexer = test_lexer("  \t\n42 \n + ");
    assert_token(&lexer, TOKEN_INT);
    assert_token(&lexer, TOKEN_PLUS);
    assert_token(&lexer, TOKEN_EOF);
}

static void test_identifiers() {
    Lexer lexer = test_lexer("variable1 variable2");
    assert_identifier(&lexer, "variable1");
    assert_identifier(&lexer, "variable2");
    assert_token(&lexer, TOKEN_EOF);
}

static void test_keywords() {
    Lexer lexer = test_lexer("let return if else func true false");
    assert_token(&lexer, TOKEN_LET);
    assert_token(&lexer, TOKEN_RETURN);
    assert_token(&lexer, TOKEN_IF);
    assert_token(&lexer, TOKEN_ELSE);
    assert_token(&lexer, TOKEN_FUNC);
    assert_token(&lexer, TOKEN_TRUE);
    assert_token(&lexer, TOKEN_FALSE);
}

static void test_errors() {
    Lexer lexer = test_lexer("42 $ +");
    assert_token(&lexer, TOKEN_INT);
    assert_token(&lexer, TOKEN_INVALID); // '$' is invalid

    // The invalid token is also reported, rather than passing silently.
    assert(diagnostics_count(&diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(&diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_SYNTAX);
    assert(strcmp(diagnostic->message, "unexpected character '$'") == 0);
    assert(diagnostic->span.line == 1);
    assert(diagnostic->span.column == 4);
}

// Valid sources must not report anything.
static void test_no_errors_on_valid_source() {
    Lexer lexer = test_lexer("let x = 1 + 2;");
    while (lexer_next(&lexer).type != TOKEN_EOF) {
    }

    assert(!diagnostics_has_errors(&diagnostics));
}

int main(void) {
    arena = arena_create(TEST_ARENA_BLOCK_SIZE);
    diagnostics_init(&diagnostics, arena, "<test>");

    test_numbers();
    test_dot_is_field_access_unless_a_digit_follows();
    test_operators();
    test_parentheses();
    test_braces();
    test_colons();
    test_whitespace();
    test_identifiers();
    test_keywords();
    test_errors();
    test_no_errors_on_valid_source();

    diagnostics_free(&diagnostics);
    arena_destroy(arena);
    return 0;
}
