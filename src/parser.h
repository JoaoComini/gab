#ifndef GAB_PARSER_H
#define GAB_PARSER_H

#include "ast.h"
#include "lexer.h"

#include <stdbool.h>

typedef struct {
    const char *message;
    size_t line;
    size_t column;
} ParseError;

typedef struct {
    Lexer *lexer;
    ParseError error;
    bool ok;
} Parser;

Parser parser_create(Lexer *lexer);

ASTScript *parser_parse(Parser *parser);

#endif
