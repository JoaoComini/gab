#include "lexer.h"
#include "string_ref.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

    return (Token){.type = TOKEN_NUMBER, .lexeme = ref};
}

static Token lexer_identifier(Lexer *lexer) {
    const char *begin = lexer->source + lexer->pos;

    while (isalnum(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    const char *end = lexer->source + lexer->pos;
    size_t length = end - begin;

    StringRef ref = {.data = begin, .length = length};

    if (strncmp(ref.data, "let", ref.length) == 0) {
        return (Token){.type = TOKEN_LET, .lexeme = ref};
    }

    if (strncmp(ref.data, "return", ref.length) == 0) {
        return (Token){.type = TOKEN_RETURN, .lexeme = ref};
    }

    return (Token){.type = TOKEN_IDENT, .lexeme = ref};
}

Lexer lexer_create(const char *source) { return (Lexer){.source = source, .pos = 0}; }

char lexer_peek(Lexer *lexer) { return lexer->source[lexer->pos]; }

void lexer_eat(Lexer *lexer) { lexer->pos++; }

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
    StringRef ref = {.data = lexer->source + lexer->pos, .length = 1};
    lexer_eat(lexer);

    switch (ch) {
    case '\0':
        return (Token){.type = TOKEN_EOF, .lexeme = ref};
    case '+':
        return (Token){.type = TOKEN_PLUS, .lexeme = ref};
    case '-':
        return (Token){.type = TOKEN_MINUS, .lexeme = ref};
    case '*':
        return (Token){.type = TOKEN_MUL, .lexeme = ref};
    case '/':
        return (Token){.type = TOKEN_DIV, .lexeme = ref};
    case '(':
        return (Token){.type = TOKEN_LPAREN, .lexeme = ref};
    case ')':
        return (Token){.type = TOKEN_RPAREN, .lexeme = ref};
    case '=':
        return (Token){.type = TOKEN_EQUAL, .lexeme = ref};
    case ';':
        return (Token){.type = TOKEN_SEMICOLON, .lexeme = ref};
    default:
        return (Token){.type = TOKEN_INVALID, .lexeme = ref};
    }
}

void lexer_unget(Lexer *lexer, Token token) {
    assert(lexer->pos >= token.lexeme.length);
    lexer->pos -= token.lexeme.length;
}
