#ifndef GAB_PARSER_H
#define GAB_PARSER_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "lexer.h"

#include <stdbool.h>

typedef struct {
    Lexer *lexer;

    Token current;

    Diagnostics *diagnostics;
} Parser;

Parser parser_create(Lexer *lexer, Diagnostics *diagnostics);

bool parser_parse(Parser *parser, ASTUnit *unit);

#endif
