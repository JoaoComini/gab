#ifndef LEXER_H
#define LEXER_H

#include "string_ref.h"

typedef enum {
    TOKEN_INVALID,   // Invalid token
    TOKEN_EOF,       // End of input
    TOKEN_NUMBER,    // All numeric literals (e.g., 42, 3.14)
                     // BEGIN OPERATORS
    TOKEN_PLUS,      // '+'
    TOKEN_MINUS,     // '-'
    TOKEN_MUL,       // '*'
    TOKEN_DIV,       // '/'
    TOKEN_ASSIGN,    // '='
    TOKEN_NOT,       // '!'
    TOKEN_LESS,      // "<"
    TOKEN_GREATER,   // ">"
    TOKEN_EQUAL,     // "=="
    TOKEN_NEQUAL,    // "!="
    TOKEN_LEQUAL,    // "<="
    TOKEN_GEQUAL,    // ">="
                     // END OPERATORS
    TOKEN_LPAREN,    // '('
    TOKEN_RPAREN,    // ')'
    TOKEN_LBRACE,    // '{'
    TOKEN_RBRACE,    // '}'
    TOKEN_SEMICOLON, // ';'
                     // BEGIN KEYWORDS
    TOKEN_LET,       // 'let'
    TOKEN_RETURN,    // 'return'
    TOKEN_IF,        // 'if'
    TOKEN_ELSE,      // 'else'
                     // END KEYWORDS
    TOKEN_IDENT,     // Variable and function names
} TokenType;

typedef struct {
    TokenType type;
    StringRef lexeme;
    int line;
    int column;
} Token;

typedef struct {
    const char *source;
    int pos;
} Lexer;

Lexer lexer_create(const char *source);
Token lexer_next(Lexer *lexer);

#endif
