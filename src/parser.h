#ifndef RULE_PARSER_H
#define RULE_PARSER_H

#include "ast.h"
#include "lexer.h"

ASTNode *parse(Lexer *lexer);

#endif
