#ifndef GAB_PARSER_H
#define GAB_PARSER_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "lexer.h"

#include <stdbool.h>

/* A '{' closes an 'if' or 'for' header, so a struct literal is spelled there only inside brackets. */
typedef enum {
    EXPR_ANY,
    EXPR_NO_STRUCT_LIT,
} ExprContext;

typedef struct {
    Arena *arena;

    Lexer *lexer;

    Token current;

    Diagnostics *diagnostics;
} Parser;

Parser parser_create(Lexer *lexer, Diagnostics *diagnostics);

bool parser_parse(Parser *parser, ASTUnit *unit);

#endif
