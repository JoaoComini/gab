#include "parser.h"
#include "ast.h"
#include "lexer.h"

#include <assert.h>
#include <stdlib.h>

static ASTNode *parse_expression(Lexer *lexer);
static ASTNode *parse_term(Lexer *lexer);
static ASTNode *parse_factor(Lexer *lexer);

static ASTBinOp parse_bin_op(TokenType type);

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
            ast_free(node);
            token_free(&token);
            return NULL;
        }

        node = new_ast_bin_op_node(node, parse_bin_op(token.type), rhs);
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
            ast_free(node);
            token_free(&token);
            return NULL;
        }

        node = new_ast_bin_op_node(node, parse_bin_op(token.type), rhs);
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
            token_free(&token);
            return NULL;
        }

        if (lexer_next(lexer).type != TOKEN_RPAREN) {
            ast_free(node);
            token_free(&token);
            return NULL;
        }

        return node;
    }

    token_free(&token);
    return NULL;
}

static ASTBinOp parse_bin_op(TokenType type) {
    switch (type) {
    case TOKEN_PLUS:
        return BIN_OP_ADD;
    case TOKEN_MINUS:
        return BIN_OP_SUB;
    case TOKEN_MUL:
        return BIN_OP_MUL;
    case TOKEN_DIV:
        return BIN_OP_DIV;
    case TOKEN_NUMBER:
    case TOKEN_LPAREN:
    case TOKEN_RPAREN:
    case TOKEN_IDENT:
    case TOKEN_EOF:
    case TOKEN_ERROR:
        break;
    }

    assert(0 && "Invalid token type");
}
