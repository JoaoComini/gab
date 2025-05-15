#ifndef LEXER_H
#define LEXER_H

typedef enum {
    TOKEN_INVALID,   // Invalid token
    TOKEN_EOF,       // End of input
    TOKEN_NUMBER,    // All numeric literals (e.g., 42, 3.14)
    TOKEN_PLUS,      // '+'
    TOKEN_MINUS,     // '-'
    TOKEN_MUL,       // '*'
    TOKEN_DIV,       // '/'
    TOKEN_LPAREN,    // '('
    TOKEN_RPAREN,    // ')'
    TOKEN_EQUAL,     // '='
    TOKEN_SEMICOLON, // ';'
    TOKEN_LET,       // 'let'
    TOKEN_IDENT,     // Variable and function names
} TokenType;

typedef struct {
    TokenType type;
    int length;
    int line;
    int column;
    union {
        double number;
        char *identifier;
    } value;
} Token;

typedef struct {
    const char *source;
    int pos;
} Lexer;

Lexer lexer_create(const char *source);
char lexer_peek(Lexer *lexer);
void lexer_eat(Lexer *lexer);
void lexer_unget(Lexer *lexer, Token token);
Token lexer_next(Lexer *lexer);

void token_free(Token *token);

#endif
