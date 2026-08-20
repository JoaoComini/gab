#ifndef LEXER_H
#define LEXER_H

#include "diagnostics.h"
#include "string/string_ref.h"

typedef enum {
    TOKEN_INVALID,     // Invalid token
    TOKEN_EOF,         // End of input
    TOKEN_INT,         // Integer literals (1, 2 ,3)
    TOKEN_FLOAT,       // Float literals (3.14)
                       // BEGIN OPERATORS
    TOKEN_PLUS,        // '+'
    TOKEN_MINUS,       // '-'
    TOKEN_MUL,         // '*'
    TOKEN_DIV,         // '/'
    TOKEN_MOD,         // '%'
    TOKEN_ASSIGN,      // '='
    TOKEN_NOT,         // '!'
    TOKEN_LESS,        // "<"
    TOKEN_GREATER,     // ">"
    TOKEN_EQUAL,       // "=="
    TOKEN_NEQUAL,      // "!="
    TOKEN_LEQUAL,      // "<="
    TOKEN_GEQUAL,      // ">="
    TOKEN_AND,         // "&&"
    TOKEN_AMP,         // '&'
    TOKEN_OR,          // "||"
                       // END OPERATORS
    TOKEN_LPAREN,      // '('
    TOKEN_RPAREN,      // ')'
    TOKEN_LBRACE,      // '{'
    TOKEN_RBRACE,      // '}'
    TOKEN_SEMICOLON,   // ';'
    TOKEN_COLON,       // ':'
    TOKEN_COLON_COLON, // '::'
    TOKEN_COMMA,       // ','
    TOKEN_DOT,         // '.'
                       // BEGIN KEYWORDS
    TOKEN_LET,         // 'let'
    TOKEN_FUNC,        // 'func'
    TOKEN_STRUCT,      // 'struct'
    TOKEN_MODULE,      // 'module'
    TOKEN_RETURN,      // 'return'
    TOKEN_IF,          // 'if'
    TOKEN_ELSE,        // 'else'
    TOKEN_FOR,         // 'for'
    TOKEN_BREAK,       // 'break'
    TOKEN_CONTINUE,    // 'continue'
    TOKEN_TRUE,        // 'true'
    TOKEN_FALSE,       // 'false'
    TOKEN_NEW,         // 'new'
    TOKEN_REF,         // 'ref'
                       // END KEYWORDS
    TOKEN_IDENT,       // Variable and function names
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
    int line;
    int column;

    // Start position of the token currently being built, so a token points at
    // its first character rather than its last.
    int start_line;
    int start_column;

    Diagnostics *diagnostics; // invalid tokens are reported here
} Lexer;

Lexer lexer_create(const char *source, Diagnostics *diagnostics);
Token lexer_next(Lexer *lexer);

Span token_span(Token token);
const char *token_description(TokenType type);

#endif
