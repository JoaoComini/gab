#include "lexer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static Token lexer_number(Lexer *lexer) {
    char *endptr;
    const char *start = lexer->source + lexer->pos;
    double value = strtod(start, &endptr); // Parse as double
    lexer->pos += (endptr - start);
    return (Token){.type = TOKEN_NUMBER, .value.number = value};
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

    return (Token){.type = TOKEN_IDENT, .value.identifier = strdup(buffer)};
}

Lexer lexer_new(const char *source) {
    return (Lexer){.source = source, .pos = 0};
}

char lexer_peek(Lexer *lexer) { return lexer->source[lexer->pos]; }

void lexer_eat(Lexer *lexer) { lexer->pos++; }

Token lexer_next(Lexer *lexer) {
    while (isspace(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    if (lexer_peek(lexer) == '\0') {
        return (Token){.type = TOKEN_EOF};
    }

    if (isdigit(lexer_peek(lexer)) || lexer_peek(lexer) == '.') {
        return lexer_number(lexer);
    }

    if (isalpha(lexer_peek(lexer))) { // Start with a letter
        return lexer_identifier(lexer);
    }

    switch (lexer_peek(lexer)) {
    case '+':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_PLUS};
    case '-':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_MINUS};
    case '*':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_MUL};
    case '/':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_DIV};
    case '(':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_LPAREN};
    case ')':
        lexer_eat(lexer);
        return (Token){.type = TOKEN_RPAREN};
    default:
        lexer_eat(lexer);
        return (Token){.type = TOKEN_ERROR};
    }
}

void token_free(Token *token) {
    switch (token->type) {
    case TOKEN_NUMBER:
    case TOKEN_PLUS:
    case TOKEN_MINUS:
    case TOKEN_MUL:
    case TOKEN_DIV:
    case TOKEN_LPAREN:
    case TOKEN_RPAREN:
    case TOKEN_EOF:
    case TOKEN_ERROR:
        break;
    case TOKEN_IDENT:
        free(token->value.identifier);
        break;
    }
}
