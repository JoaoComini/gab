#ifndef GAB_PARSER_H
#define GAB_PARSER_H

#include "ast/ast.h"
#include "lexer.h"

#include <stdbool.h>

typedef struct {
    const char *message;
    size_t line;
    size_t column;
} ParseError;

typedef struct {
    Lexer *lexer;

    Token current;

    ParseError error;
} Parser;

Parser parser_create(Lexer *lexer);

bool parser_parse(Parser *parser, ASTScript *script);

#endif
