#include "parser.h"
#include "ast.h"
#include "lexer.h"
#include "string_ref.h"
#include "variant.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

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

Token parser_peek_token(Parser *parser) {
    if (parser->has_lookahead) {
        return parser->lookahead;
    }

    parser->lookahead = lexer_next(parser->lexer);
    parser->has_lookahead = true;
    return parser->lookahead;
}

Token parser_next_token(Parser *parser) {
    if (!parser->has_lookahead) {
        return lexer_next(parser->lexer);
    }

    parser->has_lookahead = false;
    return parser->lookahead;
}

ASTScript *parser_parse(Parser *parser) {
    ASTScript *script = ast_script_create();

    while (parser_peek_token(parser).type != TOKEN_EOF) {
        Token token = parser_peek_token(parser);

        if (token.type == TOKEN_SEMICOLON) {
            parser_next_token(parser);
            continue;
        }

        ASTStmt *stmt = parse_statement(parser);
        if (!stmt) {
            ast_script_free(script);
            return NULL;
        }

        Token terminator = parser_next_token(parser);
        if (terminator.type != TOKEN_SEMICOLON) {
            parser_error(parser, "expected ';' after statement", terminator);
            ast_stmt_free(stmt);
            ast_script_free(script);

            return NULL;
        }

        ast_script_add_statement(script, stmt);
    }

    return script;
}

static ASTStmt *parse_statement(Parser *parser) {
    Token token = parser_peek_token(parser);

    switch (token.type) {
    case TOKEN_LET: {
        parser_next_token(parser); // eat "let"

        Token name = parser_next_token(parser);
        if (name.type != TOKEN_IDENT) {
            parser_error(parser, "expected identifier after 'let'", name);
            return NULL;
        }

        Token next = parser_peek_token(parser);
        if (next.type == TOKEN_SEMICOLON) {
            return ast_var_decl_stmt_create(name.lexeme, NULL);
        }

        if (next.type != TOKEN_EQUAL) {
            parser_error(parser, "expected ';' or '='", next);
            return NULL;
        }

        parser_next_token(parser); // eat '='

        ASTExpr *initializer = parse_expression(parser);
        if (!initializer) {
            return NULL;
        }

        return ast_var_decl_stmt_create(name.lexeme, initializer);
    }
    case TOKEN_IDENT: {
        ASTExpr *expr = parse_expression(parser);
        if (expr == NULL) {
            return NULL;
        }

        Token next = parser_peek_token(parser);
        if (next.type != TOKEN_EQUAL) {
            return ast_expr_stmt_create(expr);
        }

        if (expr->type != EXPR_VARIABLE) {
            parser_error(parser, "expression is not assignable", next);
            ast_expr_free(expr);
            return NULL;
        }

        parser_next_token(parser);

        ASTExpr *value = parse_expression(parser);
        if (!value) {
            return NULL;
        }

        return ast_assign_stmt_create(expr, value);
    }
    case TOKEN_RETURN: {
        parser_next_token(parser); // eat "return"

        ASTExpr *result = parse_expression(parser);
        if (!result) {
            return NULL;
        }
        return ast_return_stmt_create(result);
    }
    default: {
        ASTExpr *expr = parse_expression(parser);
        if (expr == NULL) {
            return NULL;
        }

        return ast_expr_stmt_create(expr);
    }
    }
}

static ASTExpr *parse_expression(Parser *parser) { return parse_precedence(parser, 0); }

static ASTExpr *parse_precedence(Parser *parser, int min_precedence) {
    ASTExpr *lhs = parse_primary(parser);
    if (!lhs)
        return NULL;

    while (1) {
        Token token = parser_peek_token(parser);
        int precedence = get_precedence(token.type);

        if (precedence == 0 || precedence < min_precedence) {
            break;
        }

        parser_next_token(parser); // eat op

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
    Token token = parser_next_token(parser);

    switch (token.type) {
    case TOKEN_NUMBER: {
        char *temp = string_ref_to_cstr(token.lexeme);
        double value = strtod(temp, NULL);
        free(temp);

        Variant variant = {.type = VARIANT_NUMBER, .number = value};
        return ast_literal_expr_create(variant);
    }
    case TOKEN_IDENT: {
        return ast_variable_expr_create(token.lexeme);
    }
    case TOKEN_LPAREN: {
        ASTExpr *node = parse_expression(parser);

        if (node == NULL) {
            return NULL;
        }

        Token next = parser_next_token(parser);
        if (next.type != TOKEN_RPAREN) {
            parser_error(parser, "expected ')'", next);
            ast_expr_free(node);
            return NULL;
        }

        return node;
    }
    default:
        parser_error(parser, "expected expression", token);
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
