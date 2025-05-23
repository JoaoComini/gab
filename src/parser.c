#include "parser.h"
#include "ast.h"
#include "lexer.h"
#include "string_ref.h"
#include "variant.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static ASTStmt *parse_statement(Parser *parser);
static ASTStmt *parse_var_decl_stmt(Parser *parser);
static ASTStmt *parse_if_stmt(Parser *parser);
static ASTStmt *parse_block_stmt(Parser *parser);
static ASTStmt *parse_return_stmt(Parser *parser);
static ASTStmt *parse_expr_stmt(Parser *parser);
static bool stmt_needs_terminator(ASTStmt *stmt);

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

void parser_next_token(Parser *parser) { parser->current = lexer_next(parser->lexer); }

ASTScript *parser_parse(Parser *parser) {
    ASTScript *script = ast_script_create();

    parser_next_token(parser);

    while (parser->current.type != TOKEN_EOF) {
        if (parser->current.type == TOKEN_SEMICOLON) {
            parser_next_token(parser);
            continue;
        }

        ASTStmt *stmt = parse_statement(parser);
        if (!stmt) {
            ast_script_free(script);
            return NULL;
        }

        ast_script_add_statement(script, stmt);
    }

    return script;
}

static ASTStmt *parse_statement(Parser *parser) {
    ASTStmt *stmt = NULL;

    switch (parser->current.type) {
    case TOKEN_LET: {
        stmt = parse_var_decl_stmt(parser);
        break;
    }
    case TOKEN_IF: {
        stmt = parse_if_stmt(parser);
        break;
    }
    case TOKEN_LBRACE: {
        stmt = parse_block_stmt(parser);
        break;
    }
    case TOKEN_RETURN: {
        stmt = parse_return_stmt(parser);
        break;
    }
    default: {
        stmt = parse_expr_stmt(parser);
        break;
    }
    }

    if (!stmt) {
        return NULL;
    }

    if (!stmt_needs_terminator(stmt)) {
        return stmt;
    }

    if (parser->current.type != TOKEN_SEMICOLON) {
        parser_error(parser, "expected ';' after statement", parser->current);
        ast_stmt_free(stmt);

        return NULL;
    }

    parser_next_token(parser);

    return stmt;
}

static ASTStmt *parse_var_decl_stmt(Parser *parser) {

    parser_next_token(parser); // eat "let"

    if (parser->current.type != TOKEN_IDENT) {
        parser_error(parser, "expected identifier after 'let'", parser->current);
        return NULL;
    }

    Token name = parser->current;

    parser_next_token(parser); // eat identifier

    if (parser->current.type == TOKEN_SEMICOLON) {
        return ast_var_decl_stmt_create(name.lexeme, NULL);
    }

    if (parser->current.type != TOKEN_ASSIGN) {
        parser_error(parser, "expected ';' or '='", parser->current);
        return NULL;
    }

    parser_next_token(parser); // eat '='

    ASTExpr *initializer = parse_expression(parser);
    if (!initializer) {
        parser_error(parser, "expected expression after '='", parser->current);
        return NULL;
    }

    return ast_var_decl_stmt_create(name.lexeme, initializer);
}

static ASTStmt *parse_if_stmt(Parser *parser) {

    parser_next_token(parser); // eat "if"

    ASTExpr *condition = parse_expression(parser);
    if (!condition) {
        parser_error(parser, "expected expression after 'if'", parser->current);
        return NULL;
    }

    ASTStmt *then_block = parse_block_stmt(parser);
    if (!then_block) {
        return NULL;
    }

    if (parser->current.type != TOKEN_ELSE) {
        return ast_if_stmt_create(condition, then_block, NULL);
    }

    parser_next_token(parser); // eat "else"
    ASTStmt *else_block = parse_block_stmt(parser);
    if (!else_block) {
        return NULL;
    }

    return ast_if_stmt_create(condition, then_block, else_block);
}

static ASTStmt *parse_block_stmt(Parser *parser) {
    if (parser->current.type != TOKEN_LBRACE) {
        parser_error(parser, "expected '{'", parser->current);
        return NULL;
    }

    parser_next_token(parser); // eat '{'

    ASTStmtList list = ast_stmt_list_create();
    while (parser->current.type != TOKEN_RBRACE) {
        ASTStmt *stmt = parse_statement(parser);

        if (!stmt) {
            return NULL;
        }

        ast_stmt_list_add(&list, stmt);
    }

    parser_next_token(parser); // eat '}'

    return ast_block_stmt_create(list);
}

static ASTStmt *parse_return_stmt(Parser *parser) {
    parser_next_token(parser); // eat "return"

    ASTExpr *result = parse_expression(parser);
    if (!result) {
        return NULL;
    }

    return ast_return_stmt_create(result);
}

static ASTStmt *parse_expr_stmt(Parser *parser) {
    ASTExpr *expr = parse_expression(parser);
    if (expr == NULL) {
        return NULL;
    }

    if (parser->current.type != TOKEN_ASSIGN) {
        return ast_expr_stmt_create(expr);
    }

    if (expr->type != EXPR_VARIABLE) {
        parser_error(parser, "expression is not assignable", parser->current);
        ast_expr_free(expr);
        return NULL;
    }

    parser_next_token(parser); // eat '='

    ASTExpr *value = parse_expression(parser);
    if (!value) {
        return NULL;
    }

    return ast_assign_stmt_create(expr, value);
}

static bool stmt_needs_terminator(ASTStmt *stmt) {
    switch (stmt->type) {
    case STMT_IF:
    case STMT_BLOCK:
        return false;
    default:
        return true;
    }
}

static ASTExpr *parse_expression(Parser *parser) { return parse_precedence(parser, 0); }

static ASTExpr *parse_precedence(Parser *parser, int min_precedence) {
    ASTExpr *lhs = parse_primary(parser);
    if (!lhs)
        return NULL;

    while (1) {
        Token token = parser->current;
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
    switch (parser->current.type) {
    case TOKEN_NUMBER: {
        char *temp = string_ref_to_cstr(parser->current.lexeme);
        double value = strtod(temp, NULL);
        free(temp);

        parser_next_token(parser); // eat number

        Variant variant = {.type = VARIANT_NUMBER, .number_var = value};
        return ast_literal_expr_create(variant);
    }
    case TOKEN_IDENT: {
        Token name = parser->current;

        parser_next_token(parser); // eat identifier

        return ast_variable_expr_create(name.lexeme);
    }
    case TOKEN_LPAREN: {
        parser_next_token(parser); // eat '('

        ASTExpr *node = parse_expression(parser);

        if (node == NULL) {
            return NULL;
        }

        if (parser->current.type != TOKEN_RPAREN) {
            parser_error(parser, "expected ')'", parser->current);
            ast_expr_free(node);
            return NULL;
        }

        parser_next_token(parser); // eat ')'

        return node;
    }
    default:
        parser_error(parser, "expected expression", parser->current);
        return NULL;
    }
}

static int get_precedence(TokenType type) {
    switch (type) {
    case TOKEN_EQUAL:
    case TOKEN_NEQUAL:
        return 1;
    case TOKEN_LESS:
    case TOKEN_GREATER:
    case TOKEN_LEQUAL:
    case TOKEN_GEQUAL:
        return 2;
    case TOKEN_PLUS:
    case TOKEN_MINUS:
        return 3;
    case TOKEN_MUL:
    case TOKEN_DIV:
        return 4;
    default:
        return 0; // Not a binary operator
    }
}

static BinOp parse_bin_op(TokenType type) {
    switch (type) {
    case TOKEN_EQUAL:
        return BIN_OP_EQUAL;
    case TOKEN_NEQUAL:
        return BIN_OP_NEQUAL;
    case TOKEN_LESS:
        return BIN_OP_LESS;
    case TOKEN_GREATER:
        return BIN_OP_GREATER;
    case TOKEN_LEQUAL:
        return BIN_OP_LEQUAL;
    case TOKEN_GEQUAL:
        return BIN_OP_GEQUAL;
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
