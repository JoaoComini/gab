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

// A doubled colon is one token, so qualifying a name by its module is distinct
// from annotating a type with one. The two must not be confusable: '::' is
// never two ':', and ': :' is never '::'.
static void test_colon_colon() {
    Lexer lexer = test_lexer("Player::Config");
    assert_identifier(&lexer, "Player");
    assert_token(&lexer, TOKEN_COLON_COLON);
    assert_identifier(&lexer, "Config");
    assert_token(&lexer, TOKEN_EOF);

    // A lone colon still annotates a type.
    lexer = test_lexer("x: int");
    assert_identifier(&lexer, "x");
    assert_token(&lexer, TOKEN_COLON);
    assert_identifier(&lexer, "int");
    assert_token(&lexer, TOKEN_EOF);

    // Separated colons stay separate.
    lexer = test_lexer(": :");
    assert_token(&lexer, TOKEN_COLON);
    assert_token(&lexer, TOKEN_COLON);
    assert_token(&lexer, TOKEN_EOF);

    // Three in a row is '::' then ':', not an error.
    lexer = test_lexer(":::");
    assert_token(&lexer, TOKEN_COLON_COLON);
    assert_token(&lexer, TOKEN_COLON);
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
    Lexer lexer = test_lexer("let return if else for break continue func true false struct module");
    assert_token(&lexer, TOKEN_LET);
    assert_token(&lexer, TOKEN_RETURN);
    assert_token(&lexer, TOKEN_IF);
    assert_token(&lexer, TOKEN_ELSE);
    assert_token(&lexer, TOKEN_FOR);
    assert_token(&lexer, TOKEN_BREAK);
    assert_token(&lexer, TOKEN_CONTINUE);
    assert_token(&lexer, TOKEN_FUNC);
    assert_token(&lexer, TOKEN_TRUE);
    assert_token(&lexer, TOKEN_FALSE);
    assert_token(&lexer, TOKEN_STRUCT);
    assert_token(&lexer, TOKEN_MODULE);
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

// A '//' comment runs to the end of the line, and the newline that ends it
// still counts for the line of the next token.
static void test_line_comment_spans_to_end_of_line() {
    Lexer lexer = test_lexer("let // x = 9\ny");
    assert_token(&lexer, TOKEN_LET);

    Token token = lexer_next(&lexer);
    assert(token.type == TOKEN_IDENT);
    assert(token.line == 2);
    assert_token(&lexer, TOKEN_EOF);

    assert(!diagnostics_has_errors(&diagnostics));
}

// A line comment may end the source, with no newline to terminate it.
static void test_line_comment_may_end_the_source() {
    Lexer lexer = test_lexer("x // trailing");
    assert_identifier(&lexer, "x");
    assert_token(&lexer, TOKEN_EOF);
}

// A block comment separates the tokens around it, and spans lines.
static void test_block_comment_spans_lines() {
    Lexer lexer = test_lexer("a /* one\ntwo */ b");
    assert_identifier(&lexer, "a");

    Token token = lexer_next(&lexer);
    assert(token.type == TOKEN_IDENT);
    assert(token.line == 2);
    assert_token(&lexer, TOKEN_EOF);
}

// Block comments do not nest: the first '*/' closes the comment.
static void test_block_comment_does_not_nest() {
    Lexer lexer = test_lexer("/* /* */ x");
    assert_identifier(&lexer, "x");
    assert_token(&lexer, TOKEN_EOF);
}

// An unterminated block comment is reported rather than silently consuming the
// rest of the file.
static void test_unterminated_block_comment_is_an_error() {
    Lexer lexer = test_lexer("x /* open");
    assert_identifier(&lexer, "x");
    assert_token(&lexer, TOKEN_EOF);

    assert(diagnostics_count(&diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(&diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_SYNTAX);
    assert(strcmp(diagnostic->message, "unterminated block comment") == 0);
    assert(diagnostic->span.line == 1);
    assert(diagnostic->span.column == 3);
}

// A lone '/' is still division.
static void test_slash_is_division() {
    Lexer lexer = test_lexer("a / b");
    assert_identifier(&lexer, "a");
    assert_token(&lexer, TOKEN_DIV);
    assert_identifier(&lexer, "b");
    assert_token(&lexer, TOKEN_EOF);
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
    test_colon_colon();
    test_whitespace();
    test_identifiers();
    test_keywords();
    test_errors();
    test_no_errors_on_valid_source();
    test_line_comment_spans_to_end_of_line();
    test_line_comment_may_end_the_source();
    test_block_comment_spans_lines();
    test_block_comment_does_not_nest();
    test_unterminated_block_comment_is_an_error();
    test_slash_is_division();

    diagnostics_free(&diagnostics);
    arena_destroy(arena);
    return 0;
}
