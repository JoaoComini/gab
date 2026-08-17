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

// Returns false if any syntax error was reported. The diagnostics sink is
// authoritative; this is a convenience.
bool parser_parse(Parser *parser, ASTScript *script);

#endif
