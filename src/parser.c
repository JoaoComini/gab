#include "parser.h"
#include "ast.h"
#include "lexer.h"

#include <stdlib.h>

static ASTNode *parse_expression(Lexer *lexer);
static ASTNode *parse_term(Lexer *lexer);
static ASTNode *parse_factor(Lexer *lexer);

ASTNode *parse(Lexer *lexer) { return parse_expression(lexer); }

static ASTNode *parse_expression(Lexer *lexer) {
    ASTNode *node = parse_term(lexer);

    if (node == NULL) {
        return NULL;
    }

    Token token;

    while (1) {
        token = lexer_next(lexer);
        if (token.type != TOKEN_PLUS && token.type != TOKEN_MINUS) {
            lexer->pos--; // "Unget" the token
            break;
        }
        ASTNode *rhs = parse_term(lexer);

        if (rhs == NULL) {
            return NULL;
        }

        node = new_ast_bin_op_node(node, token.type, rhs);
    }
    return node;
}

static ASTNode *parse_term(Lexer *lexer) {
    ASTNode *node = parse_factor(lexer);

    if (node == NULL) {
        return NULL;
    }

    Token token;

    while (1) {
        token = lexer_next(lexer);
        if (token.type != TOKEN_MUL && token.type != TOKEN_DIV) {
            lexer->pos--; // "Unget" the token
            break;
        }

        ASTNode *rhs = parse_factor(lexer);

        if (rhs == NULL) {
            return NULL;
        }

        node = new_ast_bin_op_node(node, token.type, rhs);
    }
    return node;
}

static ASTNode *parse_factor(Lexer *lexer) {
    Token token = lexer_next(lexer);

    if (token.type == TOKEN_NUMBER) {
        return new_ast_number_node(token.value.number);
    }

    if (token.type == TOKEN_IDENT) {
        return new_ast_variable_node(token.value.identifier);
    }

    if (token.type == TOKEN_LPAREN) {
        ASTNode *node = parse_expression(lexer);

        if (node == NULL) {
            return NULL;
        }

        if (lexer_next(lexer).type != TOKEN_RPAREN) {
            return NULL;
        }

        return node;
    }

    return NULL;
}
