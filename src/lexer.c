#include "lexer.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static Token lexer_number(Lexer *lexer) {
    const char *start = lexer->source + lexer->pos;
    char *end;
    double value = strtod(start, &end); // Parse as double

    size_t length = end - start;
    lexer->pos += length;
    return (Token){.type = TOKEN_NUMBER, .value.number = value, .length = length};
}

static Token lexer_identifier(Lexer *lexer) {
    char buffer[256];

    int i = 0;
    while (isalnum(lexer_peek(lexer))) {
        buffer[i] = lexer_peek(lexer);
        lexer_eat(lexer);
        i += 1;
    }

    buffer[i] = '\0';

    if (strcmp(buffer, "let") == 0) {
        return (Token){.type = TOKEN_LET, .length = i};
    }

    if (strcmp(buffer, "return") == 0) {
        return (Token){.type = TOKEN_RETURN, .length = i};
    }

    return (Token){.type = TOKEN_IDENT, .value.identifier = strdup(buffer), .length = i};
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

    switch (lexer_peek(lexer)) {
    case '\0':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_EOF, .length = 1};
    case '+':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_PLUS, .length = 1};
    case '-':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_MINUS, .length = 1};
    case '*':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_MUL, .length = 1};
    case '/':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_DIV, .length = 1};
    case '(':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_LPAREN, .length = 1};
    case ')':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_RPAREN, .length = 1};
    case '=':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_EQUAL, .length = 1};
    case ';':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_SEMICOLON, .length = 1};
    default:
        lexer_eat(lexer);
        return (Token){.type = TOKEN_INVALID, .length = 1};
    }
}

void lexer_unget(Lexer *lexer, Token token) {
    assert(lexer->pos >= token.length);
    lexer->pos -= token.length;
}

void token_free(Token *token) {
    switch (token->type) {
    case TOKEN_IDENT:
        free(token->value.identifier);
        break;
    default:
        break;
    }
}
