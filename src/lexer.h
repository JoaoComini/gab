#ifndef LEXER_H
#define LEXER_H

#include "arena.h"
#include "diagnostics.h"
#include "string/string.h"
#include "string/string_pool.h"
#include "string/string_ref.h"

typedef enum {
    TOKEN_INVALID,
    TOKEN_EOF,
    TOKEN_INT,
    TOKEN_FLOAT,
    TOKEN_STRING,

    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_MUL,
    TOKEN_DIV,
    TOKEN_MOD,
    TOKEN_ASSIGN,
    TOKEN_PLUS_EQ,
    TOKEN_MINUS_EQ,
    TOKEN_MUL_EQ,
    TOKEN_DIV_EQ,
    TOKEN_MOD_EQ,
    TOKEN_NOT,
    TOKEN_LESS,
    TOKEN_GREATER,
    TOKEN_EQUAL,
    TOKEN_NEQUAL,
    TOKEN_LEQUAL,
    TOKEN_GEQUAL,
    TOKEN_AND,
    TOKEN_AMP,
    TOKEN_OR,

    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_SEMICOLON,
    TOKEN_COLON,
    TOKEN_COLON_COLON,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_DOT_DOT,

    TOKEN_LET,
    TOKEN_FUNC,
    TOKEN_EXTERN,
    TOKEN_STRUCT,
    TOKEN_IMPL,
    TOKEN_INTERFACE,
    TOKEN_AS,
    TOKEN_MODULE,
    TOKEN_IMPORT,
    TOKEN_RETURN,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_BOX,

    TOKEN_IDENT,
} TokenType;

typedef union {
    int32_t as_int;
    float as_float;
    String *as_string;
} TokenValue;

typedef struct {
    TokenType type;

    StringRef lexeme;

    TokenValue value;

    int line;
    int column;
} Token;

typedef struct {
    const char *source;
    int pos;
    int line;
    int column;

    int start_line;
    int start_column;

    Diagnostics *diagnostics;

    Arena *arena;
    StringPool *strings;

    char *scratch;
    size_t scratch_capacity;
} Lexer;

Lexer lexer_create(const char *source, Arena *arena, StringPool *strings, Diagnostics *diagnostics);
Token lexer_next(Lexer *lexer);

Span token_span(Token token);
const char *token_description(TokenType type);

#endif
