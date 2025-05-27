#include "lexer.h"
#include "string/string.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char lexer_peek(Lexer *lexer) { return lexer->source[lexer->pos]; }

void lexer_eat(Lexer *lexer) { lexer->pos++; }

Token token_create(TokenType type) { return (Token){.type = type}; }
Token token_create_ref(TokenType type, StringRef ref) { return (Token){.type = type, .lexeme = ref}; }

static Token lexer_number(Lexer *lexer) {
    const char *begin = lexer->source + lexer->pos;

    while (isdigit(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    TokenType type = TOKEN_INT;
    if (lexer_peek(lexer) == '.') {
        lexer_eat(lexer);

        while (isdigit(lexer_peek(lexer))) {
            lexer_eat(lexer);
        }

        type = TOKEN_FLOAT;
    }

    const char *end = lexer->source + lexer->pos;
    size_t length = end - begin;

    StringRef ref = {.data = begin, .length = length};

    return token_create_ref(type, ref);
}

static Token lexer_identifier(Lexer *lexer) {
    const char *begin = lexer->source + lexer->pos;

    while (isalnum(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    const char *end = lexer->source + lexer->pos;
    size_t length = end - begin;

    StringRef ref = {.data = begin, .length = length};

    if (string_ref_equals_cstr(ref, "let")) {
        return token_create_ref(TOKEN_LET, ref);
    }

    if (string_ref_equals_cstr(ref, "if")) {
        return token_create_ref(TOKEN_IF, ref);
    }

    if (string_ref_equals_cstr(ref, "else")) {
        return token_create_ref(TOKEN_ELSE, ref);
    }

    if (string_ref_equals_cstr(ref, "func")) {
        return token_create_ref(TOKEN_FUNC, ref);
    }

    if (string_ref_equals_cstr(ref, "return")) {
        return token_create_ref(TOKEN_RETURN, ref);
    }

    if (string_ref_equals_cstr(ref, "true")) {
        return token_create_ref(TOKEN_TRUE, ref);
    }

    if (string_ref_equals_cstr(ref, "false")) {
        return token_create_ref(TOKEN_FALSE, ref);
    }

    return token_create_ref(TOKEN_IDENT, ref);
}

Lexer lexer_create(const char *source) { return (Lexer){.source = source, .pos = 0}; }

Token lexer_handle_eq(Lexer *lexer, TokenType base_tok, TokenType eq_tok) {
    if (lexer_peek(lexer) == '=') {
        lexer_eat(lexer);
        return token_create(eq_tok);
    }
    return token_create(base_tok);
}

Token lexer_handle_op_eq(Lexer *lexer, TokenType base_tok, TokenType eq_tok, char op_ch, TokenType op_tok) {
    char ch = lexer_peek(lexer);
    if (ch == '=') {
        lexer_eat(lexer);
        return token_create(eq_tok);
    }

    if (ch == op_ch) {
        lexer_eat(lexer);
        return token_create(op_tok);
    }

    return token_create(base_tok);
}

Token lexer_next(Lexer *lexer) {
    while (isspace(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    if (isdigit(lexer_peek(lexer)) || lexer_peek(lexer) == '.') {
        return lexer_number(lexer);
    }

    if (isalpha(lexer_peek(lexer))) { // Start with a letter
        return lexer_identifier(lexer);
    }

    char ch = lexer_peek(lexer);
    lexer_eat(lexer);

    switch (ch) {
    case '\0':
        return token_create(TOKEN_EOF);
    case '+':
        return token_create(TOKEN_PLUS);
    case '-':
        return token_create(TOKEN_MINUS);
    case '*':
        return token_create(TOKEN_MUL);
    case '/':
        return token_create(TOKEN_DIV);
    case '(':
        return token_create(TOKEN_LPAREN);
    case ')':
        return token_create(TOKEN_RPAREN);
    case '{':
        return token_create(TOKEN_LBRACE);
    case '}':
        return token_create(TOKEN_RBRACE);
    case '=':
        return lexer_handle_eq(lexer, TOKEN_ASSIGN, TOKEN_EQUAL);
    case '!':
        return lexer_handle_eq(lexer, TOKEN_NOT, TOKEN_NEQUAL);
    case '<':
        return lexer_handle_eq(lexer, TOKEN_LESS, TOKEN_LEQUAL);
    case '>':
        return lexer_handle_eq(lexer, TOKEN_GREATER, TOKEN_GEQUAL);
    case '&':
        return lexer_handle_op_eq(lexer, TOKEN_INVALID, TOKEN_INVALID, '&', TOKEN_AND);
    case '|':
        return lexer_handle_op_eq(lexer, TOKEN_INVALID, TOKEN_INVALID, '|', TOKEN_OR);
    case ';':
        return token_create(TOKEN_SEMICOLON);
    case ':':
        return token_create(TOKEN_COLON);
    case ',':
        return token_create(TOKEN_COMMA);
    default:
        return token_create(TOKEN_INVALID);
    }
}
