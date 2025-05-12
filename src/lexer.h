#ifndef LEXER_H
#define LEXER_H

#include <stdlib.h>

typedef enum {
    TOKEN_NUMBER, // All numeric literals (e.g., 42, 3.14)
    TOKEN_PLUS,   // '+'
    TOKEN_MINUS,  // '-'
    TOKEN_MUL,    // '*'
    TOKEN_DIV,    // '/'
    TOKEN_LPAREN, // '('
    TOKEN_RPAREN, // ')'
    TOKEN_IDENT,  // Variable and function names
    TOKEN_EOF,    // End of input
    TOKEN_ERROR   // Invalid token
} TokenType;

typedef struct {
    TokenType type;
    union {
        double number;
        char *identifier;
    } value;
} Token;

typedef struct {
    const char *source;
    size_t pos;
} Lexer;

Lexer lexer_new(const char *source);
char lexer_peek(Lexer *lexer);
void lexer_eat(Lexer *lexer);
Token lexer_next(Lexer *lexer);

#endif
