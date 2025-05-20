#include "lexer.h"
#include "string_ref.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char lexer_peek(Lexer *lexer) { return lexer->source[lexer->pos]; }

void lexer_eat(Lexer *lexer) { lexer->pos++; }

Token token_create(TokenType type) { return (Token){.type = type}; }
Token token_create_ref(TokenType type, StringRef ref) {
    return (Token){.type = type, .lexeme = ref};
}

static Token lexer_number(Lexer *lexer) {
    const char *begin = lexer->source + lexer->pos;

    while (isdigit(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    if (lexer_peek(lexer) == '.') {
        lexer_eat(lexer);

        while (isdigit(lexer_peek(lexer))) {
            lexer_eat(lexer);
        }
    }

    const char *end = lexer->source + lexer->pos;
    size_t length = end - begin;

    StringRef ref = {.data = begin, .length = length};

    return token_create_ref(TOKEN_NUMBER, ref);
}

static Token lexer_identifier(Lexer *lexer) {
    const char *begin = lexer->source + lexer->pos;

    while (isalnum(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    const char *end = lexer->source + lexer->pos;
    size_t length = end - begin;

    StringRef ref = {.data = begin, .length = length};

    if (ref.length == 3 && strncmp(ref.data, "let", ref.length) == 0) {
        return token_create_ref(TOKEN_LET, ref);
    }

    if (ref.length == 2 && strncmp(ref.data, "if", ref.length) == 0) {
        return token_create_ref(TOKEN_IF, ref);
    }

    if (ref.length == 4 && strncmp(ref.data, "else", ref.length) == 0) {
        return token_create_ref(TOKEN_ELSE, ref);
    }

    if (ref.length == 6 && strncmp(ref.data, "return", ref.length) == 0) {
        return token_create_ref(TOKEN_RETURN, ref);
    }

    return token_create_ref(TOKEN_IDENT, ref);
}

Lexer lexer_create(const char *source) { return (Lexer){.source = source, .pos = 0}; }

Token lexer_handle_eq(Lexer *lexer, TokenType base_token, TokenType eq_tok) {
    if (lexer_peek(lexer) == '=') {
        lexer_eat(lexer);
        return token_create(eq_tok);
    }
    return token_create(base_token);
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
    case ';':
        return token_create(TOKEN_SEMICOLON);
    default:
        return token_create(TOKEN_INVALID);
    }
}
