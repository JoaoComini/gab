#include "parser.h"
#include "ast.h"
#include "lexer.h"
#include "variant.h"

#include <assert.h>
#include <stdlib.h>

static ASTStmt *parse_statement(Parser *parser);
static ASTExpr *parse_expression(Parser *parser);
static ASTExpr *parse_primary(Parser *parser);
static ASTExpr *parse_precedence(Parser *parser, int min_precedence);
static int get_precedence(TokenType type);
static BinOp parse_bin_op(TokenType type);

static void parser_error(Parser *parser, const char *message, Token token);

Parser parser_create(Lexer *lexer) {
    return (Parser){
        .lexer = lexer,
        .ok = true,
    };
}

ASTScript *parser_parse(Parser *parser) {
    ASTScript *script = ast_script_create();

    while (1) {
        Token token = lexer_next(parser->lexer);
        if (token.type == TOKEN_EOF) {
            break;
        }

        if (token.type == TOKEN_SEMICOLON) {
            continue;
        }

        lexer_unget(parser->lexer, token);

        ASTStmt *stmt = parse_statement(parser);
        if (!stmt) {
            ast_script_free(script);
            return NULL;
        }

        Token terminator = lexer_next(parser->lexer);
        if (terminator.type != TOKEN_SEMICOLON) {
            parser_error(parser, "expected ';' after statement", terminator);
            ast_script_free(script);
            token_free(&terminator);

            return NULL;
        }

        ast_script_add_statement(script, stmt);
    }

    return script;
}

static ASTStmt *parse_statement(Parser *parser) {
    Token token = lexer_next(parser->lexer);

    switch (token.type) {
    case TOKEN_LET: {
        Token name = lexer_next(parser->lexer);
        if (name.type != TOKEN_IDENT) {
            parser_error(parser, "expected identifier after 'let'", name);
            return NULL;
        }

        Token next = lexer_next(parser->lexer);
        if (next.type == TOKEN_SEMICOLON) {
            lexer_unget(parser->lexer, next);
            return ast_var_decl_stmt_create(name.value.identifier, NULL);
        }

        if (next.type != TOKEN_EQUAL) {
            parser_error(parser, "expected ';' or '='", next);
            token_free(&name);
            token_free(&next);
            return NULL;
        }

        ASTExpr *initializer = parse_expression(parser);
        if (!initializer) {
            token_free(&name);
            return NULL;
        }

        return ast_var_decl_stmt_create(name.value.identifier, initializer);
    }
    case TOKEN_IDENT: {
        Token next = lexer_next(parser->lexer);
        if (next.type == TOKEN_EQUAL) {
            ASTExpr *value = parse_expression(parser);
            if (!value) {
                token_free(&token);
                return NULL;
            }
            ASTExpr *target = ast_variable_expr_create(token.value.identifier);
            return ast_assign_stmt_create(target, value);
        }
        lexer_unget(parser->lexer, next);
        lexer_unget(parser->lexer, token);

        ASTExpr *expr = parse_expression(parser);
        if (expr == NULL) {
            return NULL;
        }

        return ast_expr_stmt_create(expr);
    }
    case TOKEN_RETURN: {
        ASTExpr *result = parse_expression(parser);
        if (!result) {
            return NULL;
        }
        return ast_return_stmt_create(result);
    }
    default: {
        lexer_unget(parser->lexer, token);
        ASTExpr *expr = parse_expression(parser);
        if (expr == NULL) {
            return NULL;
        }

        return ast_expr_stmt_create(expr);
    }
    }
}

static ASTExpr *parse_expression(Parser *parser) { return parse_precedence(parser, 0); }

// Precedence climbing for binary operations
static ASTExpr *parse_precedence(Parser *parser, int min_precedence) {
    ASTExpr *lhs = parse_primary(parser);
    if (!lhs)
        return NULL;

    while (1) {
        Token token = lexer_next(parser->lexer);
        int precedence = get_precedence(token.type);

        if (precedence == 0 || precedence < min_precedence) {
            lexer_unget(parser->lexer, token);
            break;
        }

        BinOp op = parse_bin_op(token.type);
        ASTExpr *rhs = parse_precedence(parser, precedence + 1);

        if (!rhs) {
            ast_expr_free(lhs);
            return NULL;
        }

        lhs = ast_bin_op_expr_create(lhs, op, rhs);
    }

    return lhs;
}

static ASTExpr *parse_primary(Parser *parser) {
    Token token = lexer_next(parser->lexer);

    switch (token.type) {
    case TOKEN_NUMBER: {
        Variant variant = {.type = VARIANT_NUMBER, .number = token.value.number};
        return ast_literal_expr_create(variant);
    }
    case TOKEN_IDENT: {
        return ast_variable_expr_create(token.value.identifier);
    }
    case TOKEN_LPAREN: {
        ASTExpr *node = parse_expression(parser);

        if (node == NULL) {
            return NULL;
        }

        Token next = lexer_next(parser->lexer);
        if (next.type != TOKEN_RPAREN) {
            parser_error(parser, "expected ')'", next);
            ast_expr_free(node);
            token_free(&next);
            return NULL;
        }

        return node;
    }
    default:
        parser_error(parser, "expected expression", token);
        token_free(&token);
        return NULL;
    }
}

static int get_precedence(TokenType type) {
    switch (type) {
    case TOKEN_PLUS:
    case TOKEN_MINUS:
        return 1;
    case TOKEN_MUL:
    case TOKEN_DIV:
        return 2;
    default:
        return 0; // Not a binary operator
    }
}

static BinOp parse_bin_op(TokenType type) {
    switch (type) {
    case TOKEN_PLUS:
        return BIN_OP_ADD;
    case TOKEN_MINUS:
        return BIN_OP_SUB;
    case TOKEN_MUL:
        return BIN_OP_MUL;
    case TOKEN_DIV:
        return BIN_OP_DIV;
    default:
        break;
    }

    assert(0 && "Invalid token type");
}

static void parser_error(Parser *parser, const char *message, Token token) {
    parser->error.message = message;
    parser->error.line = token.line;
    parser->error.column = token.column;
    parser->ok = false;
}
