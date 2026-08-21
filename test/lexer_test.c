#include <arena.h>
#include <assert.h>
#include <diagnostics.h>
#include <lexer.h>
#include <string.h>
#include <string/string_pool.h>

#define TEST_ARENA_BLOCK_SIZE 2048

static Arena *arena = NULL;
static StringPool strings;
static Diagnostics diagnostics;

// Every case lexes into a fresh sink, so a test can assert on exactly what its
// own source produced.
static Lexer test_lexer(const char *source) {
    diagnostics_free(&diagnostics);
    diagnostics_init(&diagnostics, arena, "<test>");

    return lexer_create(source, arena, &strings, &diagnostics);
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
    Lexer lexer = test_lexer("+ - * / % = += -= *= /= %= ! < > == != <= >= && ||");
    assert_token(&lexer, TOKEN_PLUS);
    assert_token(&lexer, TOKEN_MINUS);
    assert_token(&lexer, TOKEN_MUL);
    assert_token(&lexer, TOKEN_DIV);
    assert_token(&lexer, TOKEN_MOD);
    assert_token(&lexer, TOKEN_ASSIGN);
    assert_token(&lexer, TOKEN_PLUS_EQ);
    assert_token(&lexer, TOKEN_MINUS_EQ);
    assert_token(&lexer, TOKEN_MUL_EQ);
    assert_token(&lexer, TOKEN_DIV_EQ);
    assert_token(&lexer, TOKEN_MOD_EQ);
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

// A string literal is one token carrying the characters it denotes.
static void test_a_string_literal_is_one_token() {
    Lexer lexer = test_lexer("\"hello\"");
    Token token = lexer_next(&lexer);

    assert(token.type == TOKEN_STRING);
    assert(token.value.as_string->length == 5);
    assert(memcmp(token.value.as_string->data, "hello", 5) == 0);

    assert_token(&lexer, TOKEN_EOF);
}

// An empty literal is a string of no characters, not a missing token.
static void test_an_empty_string_literal_lexes() {
    Lexer lexer = test_lexer("\"\"");
    Token token = lexer_next(&lexer);

    assert(token.type == TOKEN_STRING);
    assert(token.value.as_string->length == 0);

    assert_token(&lexer, TOKEN_EOF);
}

// A literal ending at the end of input is reported: the closing quote is what
// says where the string stops, so without it there is no token to hand on.
static void test_an_unterminated_string_is_an_error() {
    Lexer lexer = test_lexer("x \"open");
    assert_identifier(&lexer, "x");
    assert_token(&lexer, TOKEN_INVALID);

    assert(diagnostics_count(&diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(&diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_SYNTAX);
    assert(strcmp(diagnostic->message, "unterminated string literal") == 0);
    assert(diagnostic->span.line == 1);
    assert(diagnostic->span.column == 3);
}

// A newline ends the line before it can end the string, so a literal never
// spans one: the error points at the quote that opened it.
static void test_a_newline_does_not_continue_a_string() {
    Lexer lexer = test_lexer("\"open\nx\"");
    assert_token(&lexer, TOKEN_INVALID);

    assert(diagnostics_count(&diagnostics) == 1);
    assert(strcmp(diagnostics_get(&diagnostics, 0)->message, "unterminated string literal") == 0);
}

// The token carries the characters the literal denotes, not the source between
// the quotes: an escape is two characters in the source and one in the string.
static void test_a_string_literal_carries_decoded_characters() {
    Lexer lexer = test_lexer("\"a\\nb\\\"c\"");
    Token token = lexer_next(&lexer);

    assert(token.type == TOKEN_STRING);
    assert(token.value.as_string->length == 5);
    assert(memcmp(token.value.as_string->data, "a\nb\"c", 5) == 0);
}

// A '\0' escape is a character like any other, so it neither ends the string
// nor shortens it.
static void test_a_null_escape_is_a_character() {
    Lexer lexer = test_lexer("\"a\\0b\"");
    Token token = lexer_next(&lexer);

    assert(token.type == TOKEN_STRING);
    assert(token.value.as_string->length == 3);
    assert(memcmp(token.value.as_string->data, "a\0b", 3) == 0);
}

// An escape the language does not define is refused rather than standing for
// the character it names, so a Windows path is a mistake rather than a string
// nobody meant.
static void test_an_unknown_escape_is_an_error() {
    Lexer lexer = test_lexer("\"C:\\path\"");
    assert_token(&lexer, TOKEN_INVALID);

    assert(diagnostics_count(&diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(&diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_SYNTAX);
    assert(strcmp(diagnostic->message, "unknown escape sequence '\\p'") == 0);
}

// A numeric token carries its value, converted where the characters are read.
static void test_a_number_token_carries_its_value() {
    Lexer lexer = test_lexer("42 3.5");

    Token integer = lexer_next(&lexer);
    assert(integer.type == TOKEN_INT);
    assert(integer.value.as_int == 42);

    Token real = lexer_next(&lexer);
    assert(real.type == TOKEN_FLOAT);
    assert(real.value.as_float == 3.5f);
}

// An integer literal too large for the type it denotes is refused. The digits
// are well formed, so what is wrong is that no int holds what they denote.
static void test_an_out_of_range_integer_is_an_error() {
    Lexer lexer = test_lexer("99999999999999");
    assert_token(&lexer, TOKEN_INVALID);

    assert(diagnostics_count(&diagnostics) == 1);

    const Diagnostic *diagnostic = diagnostics_get(&diagnostics, 0);
    assert(diagnostic->kind == GAB_ERR_TYPE);
    assert(strcmp(diagnostic->message, "integer literal is out of range") == 0);
}

// The largest value the type holds is not out of range.
static void test_the_largest_integer_literal_lexes() {
    Lexer lexer = test_lexer("2147483647");

    Token token = lexer_next(&lexer);
    assert(token.type == TOKEN_INT);
    assert(token.value.as_int == 2147483647);

    assert(diagnostics_count(&diagnostics) == 0);
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
    string_pool_init(&strings, arena);
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
    test_a_string_literal_is_one_token();
    test_an_empty_string_literal_lexes();
    test_an_unterminated_string_is_an_error();
    test_a_newline_does_not_continue_a_string();
    test_a_string_literal_carries_decoded_characters();
    test_a_null_escape_is_a_character();
    test_an_unknown_escape_is_an_error();
    test_a_number_token_carries_its_value();
    test_an_out_of_range_integer_is_an_error();
    test_the_largest_integer_literal_lexes();
    test_slash_is_division();

    diagnostics_free(&diagnostics);
    string_pool_free(&strings);
    arena_destroy(arena);
    return 0;
}
