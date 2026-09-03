#include "parser.h"
#include "ast/ast.h"
#include "ast/stmt.h"
#include "lexer.h"
#include "string/string_ref.h"
#include "type/type.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static ASTStmt *parse_decl_statement(Parser *parser);
static ASTStmt *parse_statement(Parser *parser);
static ASTStmt *parse_var_decl_stmt(Parser *parser, ExprContext ctx);
static ASTStmt *parse_func_decl_stmt(Parser *parser);
static ASTStmt *parse_func_signature_stmt(Parser *parser);
static ASTStmt *parse_struct_decl_stmt(Parser *parser);
static ASTStmt *parse_impl_stmt(Parser *parser);
static ASTStmt *parse_interface_decl_stmt(Parser *parser);
static ASTField *parse_field(Parser *parser, const char *name_message);
static TypeExpr *parse_type_expr(Parser *parser);
static ASTExpr *parse_index_expr(Parser *parser, ASTExpr *target);
static ASTExpr *parse_expression(Parser *parser, ExprContext ctx);
static ASTStmt *parse_if_stmt(Parser *parser);
static ASTStmt *parse_for_stmt(Parser *parser);
static ASTStmt *parse_jump_stmt(Parser *parser);
static ASTStmt *parse_block_stmt(Parser *parser);
static ASTStmt *parse_return_stmt(Parser *parser);
static ASTStmt *parse_expr_stmt(Parser *parser, ExprContext ctx);
static bool stmt_needs_terminator(ASTStmt *stmt);

static ASTExpr *parse_expression(Parser *parser, ExprContext ctx);
static ASTExpr *parse_primary(Parser *parser);
static ASTExpr *parse_unary(Parser *parser, ExprContext ctx);
static ASTExpr *parse_field_expr(Parser *parser, ASTExpr *target);
static ASTExpr *parse_method_call_expr(Parser *parser, ASTExpr *receiver, StringRef name, Span span);
static void parser_synchronize(Parser *parser);
static ASTExpr *parse_precedence(Parser *parser, int min_precedence, ExprContext ctx);
static int get_precedence(TokenType type);
static BinOp parse_bin_op(TokenType type);

static bool parser_expect(Parser *parser, TokenType token, const char *message);
static void parser_error_found(Parser *parser, const char *message);
static void parser_error(Parser *parser, const char *message);

Parser parser_create(Lexer *lexer, Diagnostics *diagnostics) {
    return (Parser){
        .arena = lexer->arena,
        .lexer = lexer,
        .diagnostics = diagnostics,
    };
}

void parser_next_token(Parser *parser) { parser->current = lexer_next(parser->lexer); }

static Span parser_span(Parser *parser) { return token_span(parser->current); }

static void parse_module_directive(Parser *parser, ASTUnit *unit) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_IDENT, "expected a module name after 'module'")) {
        return;
    }

    StringRef name = parser->current.lexeme;

    parser_next_token(parser);

    if (parser->current.type == TOKEN_COLON_COLON) {
        const char *begin = name.data;
        size_t length = name.length;

        while (parser->current.type == TOKEN_COLON_COLON) {
            parser_next_token(parser);

            if (parser->current.type != TOKEN_IDENT) {
                break;
            }

            length = (size_t)(parser->current.lexeme.data - begin) + parser->current.lexeme.length;
            parser_next_token(parser);
        }

        diag_error(parser->diagnostics, GAB_ERR_SYNTAX, span,
                   "module names cannot be nested; '%.*s' must be a single identifier", (int)length, begin);

        return;
    }

    unit->module_name = name;
    unit->module_span = span;

    if (parser_expect(parser, TOKEN_SEMICOLON, "expected ';' after the module name")) {
        parser_next_token(parser);
    }
}

static void parse_import_directive(Parser *parser, ASTUnit *unit) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_IDENT, "expected a module name after 'import'")) {
        return;
    }

    StringRef name = parser->current.lexeme;

    parser_next_token(parser);

    if (parser->current.type == TOKEN_COLON_COLON) {
        diag_error(parser->diagnostics, GAB_ERR_SYNTAX, span,
                   "module names cannot be nested; '%.*s' must be a single identifier", (int)name.length,
                   name.data);

        return;
    }

    ast_import_list_add(&unit->imports, (ASTImport){.name = name, .span = span});

    if (parser_expect(parser, TOKEN_SEMICOLON, "expected ';' after the module name")) {
        parser_next_token(parser);
    }
}

bool parser_parse(Parser *parser, ASTUnit *unit) {
    size_t errors_before = diagnostics_count(parser->diagnostics);

    parser_next_token(parser);

    if (parser->current.type == TOKEN_MODULE) {
        parse_module_directive(parser, unit);
    } else {
        diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser),
                   "a unit must name its module: write 'module <name>;' before anything else");
    }

    while (parser->current.type == TOKEN_IMPORT) {
        parse_import_directive(parser, unit);
    }

    while (parser->current.type != TOKEN_EOF) {
        if (parser->current.type == TOKEN_SEMICOLON) {
            parser_next_token(parser);
            continue;
        }

        if (parser->current.type == TOKEN_IMPORT) {
            diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser),
                       "'import' must appear before any declaration");

            ASTUnit discarded = *unit;
            parse_import_directive(parser, &discarded);
            continue;
        }

        if (parser->current.type == TOKEN_MODULE) {
            diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser),
                       "'module' must appear once, before any declaration");

            ASTUnit discarded = *unit;
            parse_module_directive(parser, &discarded);
            continue;
        }

        ASTStmt *stmt = parse_decl_statement(parser);
        if (!stmt) {
            TokenType before = parser->current.type;
            int before_pos = parser->lexer->pos;

            parser_synchronize(parser);

            if (parser->current.type == before && parser->lexer->pos == before_pos) {
                parser_next_token(parser);
            }

            continue;
        }

        ast_unit_add_statement(unit, stmt);
    }

    return diagnostics_count(parser->diagnostics) == errors_before;
}

static void parser_synchronize(Parser *parser) {
    while (parser->current.type != TOKEN_EOF) {
        if (parser->current.type == TOKEN_SEMICOLON) {
            parser_next_token(parser);
            return;
        }

        if (parser->current.type == TOKEN_RBRACE) {
            return;
        }

        switch (parser->current.type) {
        case TOKEN_LET:
        case TOKEN_FUNC:
        case TOKEN_EXTERN:
        case TOKEN_STRUCT:
        case TOKEN_IMPL:
        case TOKEN_MODULE:
        case TOKEN_IF:
        case TOKEN_FOR:
        case TOKEN_BREAK:
        case TOKEN_CONTINUE:
        case TOKEN_RETURN:
            return;
        default:
            parser_next_token(parser);
            break;
        }
    }
}

static ASTStmt *parse_decl_statement(Parser *parser) {
    ASTStmt *stmt = NULL;

    switch (parser->current.type) {
    case TOKEN_LET: {
        stmt = parse_var_decl_stmt(parser, EXPR_ANY);
        break;
    }
    case TOKEN_FUNC:
    case TOKEN_EXTERN: {
        stmt = parse_func_decl_stmt(parser);
        break;
    }
    case TOKEN_STRUCT: {
        stmt = parse_struct_decl_stmt(parser);
        break;
    }
    case TOKEN_IMPL: {
        stmt = parse_impl_stmt(parser);
        break;
    }
    case TOKEN_INTERFACE: {
        stmt = parse_interface_decl_stmt(parser);
        break;
    }
    default: {
        parser_error_found(
            parser, "expected a declaration ('let', 'func', 'extern', 'struct', 'impl', or 'interface')");
        return NULL;
    }
    }

    if (!stmt) {
        return NULL;
    }

    if (!stmt_needs_terminator(stmt)) {
        return stmt;
    }

    if (!parser_expect(parser, TOKEN_SEMICOLON, "expected ';'")) {
        return NULL;
    }

    parser_next_token(parser);

    return stmt;
}

static ASTStmt *parse_statement(Parser *parser) {
    ASTStmt *stmt = NULL;

    switch (parser->current.type) {
    case TOKEN_LET: {
        stmt = parse_var_decl_stmt(parser, EXPR_ANY);
        break;
    }
    case TOKEN_FUNC: {
        parser_error(parser, "a function cannot be declared inside another; declare it at module level");

        parse_func_decl_stmt(parser);

        return NULL;
    }
    case TOKEN_STRUCT: {
        stmt = parse_struct_decl_stmt(parser);
        break;
    }
    case TOKEN_IMPL: {
        parser_error(parser,
                     "an 'impl' block cannot be declared inside a function; declare it at module level");

        parse_impl_stmt(parser);

        return NULL;
    }
    case TOKEN_INTERFACE: {
        parser_error(parser,
                     "an 'interface' cannot be declared inside a function; declare it at module level");

        parse_interface_decl_stmt(parser);

        return NULL;
    }
    case TOKEN_IF: {
        stmt = parse_if_stmt(parser);
        break;
    }
    case TOKEN_FOR: {
        stmt = parse_for_stmt(parser);
        break;
    }
    case TOKEN_BREAK:
    case TOKEN_CONTINUE: {
        stmt = parse_jump_stmt(parser);
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
        stmt = parse_expr_stmt(parser, EXPR_ANY);
        break;
    }
    }

    if (!stmt) {
        return NULL;
    }

    if (!stmt_needs_terminator(stmt)) {
        return stmt;
    }

    if (!parser_expect(parser, TOKEN_SEMICOLON, "expected ';'")) {
        return NULL;
    }

    parser_next_token(parser);

    return stmt;
}

static ASTStmt *parse_var_decl_stmt(Parser *parser, ExprContext ctx) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_IDENT, "expected a variable name after 'let'")) {
        return NULL;
    }

    Token name = parser->current;

    parser_next_token(parser);

    TypeExpr *spec = NULL;
    if (parser->current.type == TOKEN_COLON) {
        parser_next_token(parser);

        spec = parse_type_expr(parser);
        if (!spec) {
            return NULL;
        }
    }

    if (parser->current.type == TOKEN_SEMICOLON) {
        if (!spec) {
            parser_error(parser, "expected a type or an initializer");
            return NULL;
        }

        return ast_var_decl_stmt_create(parser->arena, span, name.lexeme, spec, NULL);
    }

    if (!parser_expect(parser, TOKEN_ASSIGN, "expected ';' or '='")) {
        if (spec) {
        }
        return NULL;
    }

    parser_next_token(parser);

    ASTExpr *initializer = parse_expression(parser, ctx);
    if (!initializer) {
        if (spec) {
        }
        return NULL;
    }

    return ast_var_decl_stmt_create(parser->arena, span, name.lexeme, spec, initializer);
}

/* Inside brackets a '{' cannot open a block, so a struct literal is spelled there even in a header. */
static ASTStmt *parse_if_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    ASTExpr *condition = parse_expression(parser, EXPR_NO_STRUCT_LIT);

    if (!condition) {
        return NULL;
    }

    ASTStmt *then_block = parse_block_stmt(parser);
    if (!then_block) {
        return NULL;
    }

    if (parser->current.type != TOKEN_ELSE) {
        return ast_if_stmt_create(parser->arena, span, condition, then_block, NULL);
    }

    parser_next_token(parser);
    ASTStmt *else_block = parse_block_stmt(parser);
    if (!else_block) {
        return NULL;
    }

    return ast_if_stmt_create(parser->arena, span, condition, then_block, else_block);
}

static ASTStmt *parse_for_clause(Parser *parser, TokenType terminator) {
    if (parser->current.type == terminator) {
        return NULL;
    }

    if (parser->current.type == TOKEN_LET) {
        return parse_var_decl_stmt(parser, EXPR_NO_STRUCT_LIT);
    }

    return parse_expr_stmt(parser, EXPR_NO_STRUCT_LIT);
}

static ASTStmt *parse_for_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    if (parser->current.type == TOKEN_LBRACE) {
        ASTStmt *body = parse_block_stmt(parser);
        if (!body) {
            return NULL;
        }

        return ast_for_stmt_create(parser->arena, span, NULL, NULL, NULL, body);
    }

    ASTStmt *init = NULL;
    ASTExpr *condition = NULL;
    ASTStmt *post = NULL;

    ASTStmt *first = parse_for_clause(parser, TOKEN_SEMICOLON);

    if (parser->current.type != TOKEN_SEMICOLON) {
        if (!first) {
            parser_error_found(parser, "expected a loop condition or '{'");
            return NULL;
        }

        if (first->kind != STMT_EXPR) {
            parser_error(parser, "a loop condition must be an expression");
            return NULL;
        }

        condition = first->expr.value;
        first->expr.value = NULL;

        ASTStmt *body = parse_block_stmt(parser);
        if (!body) {
            return NULL;
        }

        return ast_for_stmt_create(parser->arena, span, NULL, condition, NULL, body);
    }

    init = first;

    parser_next_token(parser);

    if (parser->current.type != TOKEN_SEMICOLON) {
        condition = parse_expression(parser, EXPR_NO_STRUCT_LIT);
        if (!condition) {
            return NULL;
        }
    }

    if (!parser_expect(parser, TOKEN_SEMICOLON, "expected ';' after the loop condition")) {
        return NULL;
    }

    parser_next_token(parser);

    post = parse_for_clause(parser, TOKEN_LBRACE);

    ASTStmt *body = parse_block_stmt(parser);
    if (!body) {
        return NULL;
    }

    return ast_for_stmt_create(parser->arena, span, init, condition, post, body);
}

static ASTStmt *parse_jump_stmt(Parser *parser) {
    Span span = parser_span(parser);
    bool is_break = parser->current.type == TOKEN_BREAK;

    parser_next_token(parser);

    return ast_jump_stmt_create(parser->arena, span, is_break);
}

static ASTStmt *parse_block_stmt(Parser *parser) {
    Span span = parser_span(parser);

    if (!parser_expect(parser, TOKEN_LBRACE, "expected '{'")) {
        return NULL;
    }

    parser_next_token(parser);

    ASTStmtList list = ast_stmt_list_create(arena_allocator(parser->arena));
    while (parser->current.type != TOKEN_RBRACE) {
        if (parser->current.type == TOKEN_EOF) {
            parser_error_found(parser, "expected '}' to close the block");
            return NULL;
        }

        ASTStmt *stmt = parse_statement(parser);

        if (!stmt) {
            parser_synchronize(parser);
            continue;
        }

        ast_stmt_list_add(&list, stmt);
    }

    parser_next_token(parser);

    return ast_block_stmt_create(parser->arena, span, list);
}

static ASTField *parse_field(Parser *parser, const char *name_message) {
    if (!parser_expect(parser, TOKEN_IDENT, name_message)) {
        return NULL;
    }

    Span span = parser_span(parser);
    StringRef name = parser->current.lexeme;
    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_COLON, "expected ':' after name")) {
        return NULL;
    }

    parser_next_token(parser);

    TypeExpr *type = parse_type_expr(parser);
    if (!type) {
        return NULL;
    }

    return ast_field_create(parser->arena, span, name, type);
}

static TypeExpr *parse_type_expr(Parser *parser) {
    if (parser->current.type == TOKEN_LBRACKET) {
        parser_next_token(parser);

        TypeExpr *element = parse_type_expr(parser);

        if (!element) {
            return NULL;
        }

        if (!parser_expect(parser, TOKEN_SEMICOLON, "expected ';' and a length, as '[int; 3]'")) {
            return NULL;
        }

        parser_next_token(parser);

        if (parser->current.type != TOKEN_INT) {
            parser_error(parser, "an array's length must be an integer literal");
            return NULL;
        }

        int32_t length = parser->current.value.as_int;

        parser_next_token(parser);

        if (!parser_expect(parser, TOKEN_RBRACKET, "expected ']' after an array's length")) {
            return NULL;
        }

        parser_next_token(parser);

        return type_expr_array(parser->arena, element, length);
    }

    /* In a type '&' borrows and '*' owns; neither is the expression operator that shares its spelling. */
    if (parser->current.type == TOKEN_AMP || parser->current.type == TOKEN_MUL ||
        parser->current.type == TOKEN_AND) {
        TypeExprKind kind = parser->current.type == TOKEN_MUL ? TYPE_EXPR_BOX : TYPE_EXPR_REF;

        /* '&&T' lexes as one token, so consuming half of it leaves the borrow it still spells. */
        if (parser->current.type == TOKEN_AND) {
            parser->current.type = TOKEN_AMP;
        } else {
            parser_next_token(parser);
        }

        TypeExpr *inner = parse_type_expr(parser);

        if (!inner) {
            return NULL;
        }

        return type_expr_indirect(parser->arena, kind, inner);
    }

    if (!parser_expect(parser, TOKEN_IDENT, "expected a type")) {
        return NULL;
    }

    StringRef name = parser->current.lexeme;
    parser_next_token(parser);

    if (parser->current.type == TOKEN_COLON_COLON) {
        parser_next_token(parser);

        if (!parser_expect(parser, TOKEN_IDENT, "expected a type name after '::'")) {
            return NULL;
        }

        StringRef member = parser->current.lexeme;
        name.length = (size_t)(member.data - name.data) + member.length;

        parser_next_token(parser);

        if (parser->current.type == TOKEN_COLON_COLON) {
            parser_error(parser, "a qualified type name has one '::', as 'Module::Type'");
            return NULL;
        }
    }

    TypeExpr *base = type_expr_name(parser->arena, name);

    TypeExpr *type = base;

    if (parser->current.type == TOKEN_LESS) {
        parser_next_token(parser);

        TypeExpr *apply = type_expr_apply(parser->arena, base);

        for (;;) {
            TypeExpr *argument = parse_type_expr(parser);

            if (!argument) {
                return NULL;
            }

            type_expr_list_add(&apply->apply.args, argument);

            if (parser->current.type != TOKEN_COMMA) {
                break;
            }

            parser_next_token(parser);
        }

        if (!parser_expect(parser, TOKEN_GREATER, "expected '>' after a type's arguments")) {
            return NULL;
        }

        parser_next_token(parser);

        type = apply;
    }

    return type;
}

static ASTStmt *parse_struct_decl_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_IDENT, "expected a struct name")) {
        return NULL;
    }

    StringRef name = parser->current.lexeme;
    parser_next_token(parser);

    StringRef params[GAB_MAX_TYPE_PARAMS];
    size_t param_count = 0;

    if (parser->current.type == TOKEN_LESS) {
        parser_next_token(parser);

        for (;;) {
            if (!parser_expect(parser, TOKEN_IDENT, "expected a type parameter name")) {
                return NULL;
            }

            if (param_count == GAB_MAX_TYPE_PARAMS) {
                diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser),
                           "a struct takes at most %d type parameters", GAB_MAX_TYPE_PARAMS);
                return NULL;
            }

            params[param_count++] = parser->current.lexeme;
            parser_next_token(parser);

            if (parser->current.type != TOKEN_COMMA) {
                break;
            }

            parser_next_token(parser);
        }

        if (!parser_expect(parser, TOKEN_GREATER, "expected '>' after a struct's type parameters")) {
            return NULL;
        }

        parser_next_token(parser);
    }

    if (!parser_expect(parser, TOKEN_LBRACE, "expected '{' after struct name")) {
        return NULL;
    }

    parser_next_token(parser);

    ASTFieldList fields = ast_field_list_create(arena_allocator(parser->arena));
    while (parser->current.type != TOKEN_RBRACE) {
        if (parser->current.type == TOKEN_EOF) {
            parser_error(parser, "expected '}' to close the struct");
            return NULL;
        }

        ASTField *field = parse_field(parser, "expected a field name");
        if (!field) {
            return NULL;
        }

        ast_field_list_add(&fields, field);

        if (parser->current.type != TOKEN_COMMA && parser->current.type != TOKEN_RBRACE) {
            parser_error_found(parser, "expected ',' or '}' after field");
            return NULL;
        }

        if (parser->current.type == TOKEN_COMMA) {
            parser_next_token(parser);
        }
    }

    parser_next_token(parser);

    return ast_struct_decl_stmt_create(parser->arena, span, name, params, param_count, fields);
}

static void func_decl_take_type_params(ASTStmt *decl, TypeExprList *params) {
    for (size_t i = 0; i < params->size && i < GAB_MAX_TYPE_PARAMS; i++) {
        if (params->data[i]->kind != TYPE_EXPR_NAME) {
            continue;
        }

        decl->func_decl.type_params[decl->func_decl.type_param_count++] = params->data[i]->name;
    }
}

static ASTStmt *parse_func_decl_stmt_inner(Parser *parser, bool signature_only) {
    Span span = parser_span(parser);

    bool is_extern = signature_only || parser->current.type == TOKEN_EXTERN;

    if (is_extern && !signature_only) {
        parser_next_token(parser);

        if (!parser_expect(parser, TOKEN_FUNC, "expected 'func' after 'extern'")) {
            return NULL;
        }
    }

    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_IDENT, "expected a function name")) {
        return NULL;
    }

    StringRef func_name = parser->current.lexeme;
    parser_next_token(parser);

    TypeExprList type_params = type_expr_list_create(arena_allocator(parser->arena));

    if (parser->current.type == TOKEN_LESS) {
        parser_next_token(parser);

        for (;;) {
            if (!parser_expect(parser, TOKEN_IDENT, "expected a type parameter name")) {
                return NULL;
            }

            type_expr_list_add(&type_params, type_expr_name(parser->arena, parser->current.lexeme));
            parser_next_token(parser);

            if (parser->current.type != TOKEN_COMMA) {
                break;
            }

            parser_next_token(parser);
        }

        if (!parser_expect(parser, TOKEN_GREATER, "expected '>' after a function's type parameters")) {
            return NULL;
        }

        parser_next_token(parser);
    }

    if (parser->current.type == TOKEN_COLON_COLON) {
        parser_error(parser, "a function on a type is declared in an 'impl' block for that type");
        return NULL;
    }

    if (!parser_expect(parser, TOKEN_LPAREN, "expected '(' after function name")) {
        return NULL;
    }

    parser_next_token(parser);

    ASTFieldList func_params = ast_field_list_create(arena_allocator(parser->arena));
    while (parser->current.type != TOKEN_RPAREN) {
        ASTField *param = parse_field(parser, "expected a parameter name");
        if (!param) {
            return NULL;
        }

        if (parser->current.type != TOKEN_COMMA && parser->current.type != TOKEN_RPAREN) {
            parser_error_found(parser, "expected ',' or ')' after parameter");
            return NULL;
        }

        if (parser->current.type == TOKEN_COMMA) {
            parser_next_token(parser);
        }

        ast_field_list_add(&func_params, param);
    }

    parser_next_token(parser);

    TypeExpr *func_type = NULL;
    if (parser->current.type == TOKEN_COLON) {
        parser_next_token(parser);

        func_type = parse_type_expr(parser);
        if (!func_type) {
            return NULL;
        }
    }

    if (is_extern) {
        if (parser->current.type == TOKEN_LBRACE) {
            parser_error(parser, signature_only
                                     ? "an interface declares a signature, which has no body"
                                     : "an 'extern' function is defined by the host and cannot have a body");

            parse_block_stmt(parser);

            return NULL;
        }

        ASTStmt *decl =
            ast_func_decl_stmt_create(parser->arena, span, func_name, func_type, func_params, NULL);

        func_decl_take_type_params(decl, &type_params);

        return decl;
    }

    ASTStmt *func_body = parse_block_stmt(parser);
    if (!func_body) {

        return NULL;
    }

    ASTStmt *decl =
        ast_func_decl_stmt_create(parser->arena, span, func_name, func_type, func_params, func_body);

    func_decl_take_type_params(decl, &type_params);

    return decl;
}

static ASTStmt *parse_func_decl_stmt(Parser *parser) { return parse_func_decl_stmt_inner(parser, false); }

static ASTStmt *parse_func_signature_stmt(Parser *parser) { return parse_func_decl_stmt_inner(parser, true); }

static ASTStmt *parse_impl_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    TypeExprList params = type_expr_list_create(arena_allocator(parser->arena));

    if (parser->current.type == TOKEN_LESS) {
        parser_next_token(parser);

        for (;;) {
            if (!parser_expect(parser, TOKEN_IDENT, "expected a type parameter name")) {
                return NULL;
            }

            type_expr_list_add(&params, type_expr_name(parser->arena, parser->current.lexeme));
            parser_next_token(parser);

            if (parser->current.type != TOKEN_COMMA) {
                break;
            }

            parser_next_token(parser);
        }

        if (!parser_expect(parser, TOKEN_GREATER, "expected '>' after an impl's type parameters")) {
            return NULL;
        }

        parser_next_token(parser);
    }

    TypeExpr *type = parse_type_expr(parser);
    if (!type) {
        return NULL;
    }

    StringRef interface_name = {0};
    Span interface_span = span;

    if (parser->current.type == TOKEN_AS) {
        parser_next_token(parser);

        if (!parser_expect(parser, TOKEN_IDENT, "expected the name of an interface after 'as'")) {
            return NULL;
        }

        interface_name = parser->current.lexeme;
        interface_span = parser_span(parser);

        parser_next_token(parser);
    }

    if (!parser_expect(parser, TOKEN_LBRACE, "expected '{' after the type an 'impl' block is for")) {
        return NULL;
    }

    parser_next_token(parser);

    ASTStmtList members = ast_stmt_list_create(arena_allocator(parser->arena));

    while (parser->current.type != TOKEN_RBRACE) {
        if (parser->current.type == TOKEN_EOF) {
            parser_error(parser, "expected '}' to close the impl block");
            return NULL;
        }

        if (parser->current.type != TOKEN_FUNC && parser->current.type != TOKEN_EXTERN) {
            parser_error_found(parser, "expected a function in an 'impl' block");
            return NULL;
        }

        ASTStmt *member = parse_func_decl_stmt(parser);
        if (!member) {
            return NULL;
        }

        if (member->func_decl.owner) {
            parser_error(parser, "a function in an 'impl' block is named without its type");
            return NULL;
        }

        member->func_decl.owner = type;

        func_decl_take_type_params(member, &params);

        if (stmt_needs_terminator(member)) {
            if (!parser_expect(parser, TOKEN_SEMICOLON, "expected ';'")) {
                return NULL;
            }

            parser_next_token(parser);
        }

        ast_stmt_list_add(&members, member);
    }

    parser_next_token(parser);

    ASTStmt *stmt = ast_impl_stmt_create(parser->arena, span, type, members);

    stmt->impl.interface_name = interface_name;
    stmt->impl.interface_span = interface_span;

    return stmt;
}

static ASTStmt *parse_interface_decl_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_IDENT, "expected an interface name")) {
        return NULL;
    }

    StringRef name = parser->current.lexeme;

    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_LBRACE, "expected '{' after an interface's name")) {
        return NULL;
    }

    parser_next_token(parser);

    ASTStmtList members = ast_stmt_list_create(arena_allocator(parser->arena));

    while (parser->current.type != TOKEN_RBRACE) {
        if (parser->current.type == TOKEN_EOF) {
            parser_error(parser, "expected '}' to close the interface");
            return NULL;
        }

        if (parser->current.type != TOKEN_FUNC) {
            parser_error_found(parser, "expected a function signature in an interface");
            return NULL;
        }

        ASTStmt *member = parse_func_signature_stmt(parser);
        if (!member) {
            return NULL;
        }

        if (!parser_expect(parser, TOKEN_SEMICOLON, "expected ';'")) {
            return NULL;
        }

        parser_next_token(parser);

        ast_stmt_list_add(&members, member);
    }

    parser_next_token(parser);

    return ast_interface_decl_stmt_create(parser->arena, span, name, members);
}

static ASTStmt *parse_return_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    ASTExpr *result = parse_expression(parser, EXPR_ANY);
    if (!result) {
        return NULL;
    }

    return ast_return_stmt_create(parser->arena, span, result);
}

static bool compound_assign_op(TokenType type, BinOp *op) {
    BinOp found;

    switch (type) {
    case TOKEN_PLUS_EQ:
        found = BIN_OP_ADD;
        break;
    case TOKEN_MINUS_EQ:
        found = BIN_OP_SUB;
        break;
    case TOKEN_MUL_EQ:
        found = BIN_OP_MUL;
        break;
    case TOKEN_DIV_EQ:
        found = BIN_OP_DIV;
        break;
    case TOKEN_MOD_EQ:
        found = BIN_OP_MOD;
        break;
    default:
        return false;
    }

    if (op) {
        *op = found;
    }

    return true;
}

static ASTStmt *parse_expr_stmt(Parser *parser, ExprContext ctx) {
    Span span = parser_span(parser);

    ASTExpr *expr = parse_expression(parser, ctx);
    if (expr == NULL) {
        return NULL;
    }

    bool compound = compound_assign_op(parser->current.type, NULL);

    if (parser->current.type != TOKEN_ASSIGN && !compound) {
        return ast_expr_stmt_create(parser->arena, span, expr);
    }

    if (expr->kind != EXPR_VARIABLE && expr->kind != EXPR_FIELD && expr->kind != EXPR_DEREF &&
        expr->kind != EXPR_INDEX) {
        parser_error(parser, "expression is not assignable");
        return NULL;
    }

    BinOp op = BIN_OP_ADD;
    compound_assign_op(parser->current.type, &op);

    parser_next_token(parser);

    ASTExpr *value = parse_expression(parser, ctx);
    if (!value) {
        return NULL;
    }

    if (compound) {
        return ast_compound_assign_stmt_create(parser->arena, span, expr, op, value);
    }

    return ast_assign_stmt_create(parser->arena, span, expr, value);
}

static bool stmt_needs_terminator(ASTStmt *stmt) {
    switch (stmt->kind) {
    case STMT_IF:
    case STMT_FOR:
    case STMT_BLOCK:
    case STMT_STRUCT_DECL:
    case STMT_IMPL:
    case STMT_INTERFACE_DECL:
        return false;

    case STMT_FUNC_DECL:
        return stmt->func_decl.body == NULL;
    default:
        return true;
    }
}

static ASTExpr *parse_expression(Parser *parser, ExprContext ctx) { return parse_precedence(parser, 0, ctx); }

static ASTExpr *parse_field_expr(Parser *parser, ASTExpr *target) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_IDENT, "expected a field name after '.'")) {
        return NULL;
    }

    Token name = parser->current;

    parser_next_token(parser);

    if (parser->current.type == TOKEN_LPAREN) {
        return parse_method_call_expr(parser, target, name.lexeme, span);
    }

    return ast_field_expr_create(parser->arena, span, target, name.lexeme);
}

static bool parse_call_args(Parser *parser, ASTExprList *out) {
    parser_next_token(parser);

    ASTExprList args = ast_expr_list_create(arena_allocator(parser->arena));
    bool ok = true;

    while (ok && parser->current.type != TOKEN_RPAREN) {
        if (parser->current.type == TOKEN_EOF) {
            parser_error(parser, "expected ')' to close the argument list");
            ok = false;
            break;
        }

        ASTExpr *arg = parse_expression(parser, EXPR_ANY);
        if (!arg) {
            ok = false;
            break;
        }

        ast_expr_list_add(&args, arg);

        if (parser->current.type != TOKEN_COMMA && parser->current.type != TOKEN_RPAREN) {
            parser_error_found(parser, "expected ',' or ')' after argument");
            ok = false;
            break;
        }

        if (parser->current.type == TOKEN_COMMA) {
            parser_next_token(parser);
        }
    }

    if (!ok) {
        return false;
    }

    parser_next_token(parser);

    *out = args;
    return true;
}

/* '<' after a name opens type arguments only when '::' or '{' closes them; otherwise it is a comparison. */
static bool parser_scan_type_args(Parser *parser, TokenType after) {
    parser_next_token(parser);

    for (int depth = 1; depth > 0;) {
        switch (parser->current.type) {
        case TOKEN_LESS:
            depth++;
            break;
        case TOKEN_GREATER:
            depth--;
            break;
        case TOKEN_IDENT:
        case TOKEN_COMMA:
        case TOKEN_AMP:
        case TOKEN_MUL:
        case TOKEN_LBRACKET:
        case TOKEN_RBRACKET:
        case TOKEN_SEMICOLON:
        case TOKEN_INT:
            break;
        /* Every token 'parse_type_expr' accepts must be listed above, or a valid type argument scans as a
         * comparison. */
        default:
            return false;
        }

        parser_next_token(parser);
    }

    return parser->current.type == after;
}

static bool parser_type_args_close_with(Parser *parser, TokenType after) {
    Lexer saved_lexer = *parser->lexer;
    Token saved_current = parser->current;

    bool ok = parser_scan_type_args(parser, after);

    *parser->lexer = saved_lexer;
    parser->current = saved_current;

    return ok;
}

static bool parser_type_args_precede_a_brace(Parser *parser) {
    return parser_type_args_close_with(parser, TOKEN_LBRACE);
}

static bool parser_type_args_precede_colons(Parser *parser) {
    return parser_type_args_close_with(parser, TOKEN_COLON_COLON);
}

/* 'f<int>(' supplies type arguments; 'a < b' compares, and only the closing token tells them apart. */
static bool parser_type_args_precede_a_call(Parser *parser) {
    return parser_type_args_close_with(parser, TOKEN_LPAREN);
}

static TypeExpr *parse_type_args_for(Parser *parser, StringRef name) {
    parser_next_token(parser);

    TypeExpr *apply = type_expr_apply(parser->arena, type_expr_name(parser->arena, name));

    for (;;) {
        TypeExpr *argument = parse_type_expr(parser);

        if (!argument) {
            return NULL;
        }

        type_expr_list_add(&apply->apply.args, argument);

        if (parser->current.type != TOKEN_COMMA) {
            break;
        }

        parser_next_token(parser);
    }

    if (!parser_expect(parser, TOKEN_GREATER, "expected '>' after a type's arguments")) {
        return NULL;
    }

    parser_next_token(parser);

    return apply;
}

static bool parse_field_inits(Parser *parser, ASTFieldInitList *out) {
    while (parser->current.type != TOKEN_RBRACE) {
        Span field_span = parser_span(parser);

        if (!parser_expect(parser, TOKEN_IDENT, "expected a field name")) {
            return false;
        }

        StringRef field_name = parser->current.lexeme;

        parser_next_token(parser);

        if (!parser_expect(parser, TOKEN_COLON, "expected ':' after a field name")) {
            return false;
        }

        parser_next_token(parser);

        ASTExpr *value = parse_expression(parser, EXPR_ANY);

        if (!value) {
            return false;
        }

        ast_field_init_list_add(out, (ASTFieldInit){.name = field_name, .value = value, .span = field_span});

        if (parser->current.type != TOKEN_COMMA) {
            break;
        }

        parser_next_token(parser);
    }

    return parser_expect(parser, TOKEN_RBRACE, "expected '}' or ',' after a field value");
}

static ASTExpr *parse_struct_lit_expr(Parser *parser, ASTExpr *target) {
    Span span = target->span;
    StringRef name = target->var.name;

    TypeExpr *type_expr = parser->current.type == TOKEN_LESS ? parse_type_args_for(parser, name)
                                                             : type_expr_name(parser->arena, name);

    if (!type_expr) {
        return NULL;
    }

    if (!parser_expect(parser, TOKEN_LBRACE, "expected '{' after a struct's name")) {
        return NULL;
    }

    parser_next_token(parser);

    ASTFieldInitList fields = ast_field_init_list_create(arena_allocator(parser->arena));

    if (!parse_field_inits(parser, &fields)) {
        return NULL;
    }

    parser_next_token(parser);

    return ast_struct_lit_expr_create(parser->arena, span, type_expr, fields);
}

static ASTExpr *parse_call_expr(Parser *parser, ASTExpr *target) {
    Span span = parser_span(parser);

    ASTExprList args;
    if (!parse_call_args(parser, &args)) {
        return NULL;
    }

    return ast_call_expr_create(parser->arena, span, target, args);
}

static ASTExpr *parse_method_call_expr(Parser *parser, ASTExpr *receiver, StringRef name, Span span) {
    ASTExpr *target = ast_field_expr_create(parser->arena, span, receiver, name);

    ASTExprList args;
    if (!parse_call_args(parser, &args)) {
        return NULL;
    }

    return ast_call_expr_create(parser->arena, span, target, args);
}

static ASTExpr *parse_index_expr(Parser *parser, ASTExpr *target) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    ASTExpr *index = parse_expression(parser, EXPR_ANY);

    if (!index) {
        return NULL;
    }

    if (!parser_expect(parser, TOKEN_RBRACKET, "expected ']' after an index")) {
        return NULL;
    }

    parser_next_token(parser);

    return ast_index_expr_create(parser->arena, span, target, index);
}

/* A '{' after a plain name opens a literal; '<' does only when the type arguments it opens reach one. */
static bool starts_struct_lit(Parser *parser, const ASTExpr *expr, ExprContext ctx) {
    if (ctx != EXPR_ANY || expr->kind != EXPR_VARIABLE) {
        return false;
    }

    return parser->current.type == TOKEN_LBRACE || parser_type_args_precede_a_brace(parser);
}

static ASTExpr *parse_postfix(Parser *parser, ASTExpr *expr, ExprContext ctx) {
    while (expr) {
        switch (parser->current.type) {
        case TOKEN_LPAREN:
            expr = parse_call_expr(parser, expr);
            break;
        case TOKEN_LBRACKET:
            expr = parse_index_expr(parser, expr);
            break;
        case TOKEN_DOT:
            expr = parse_field_expr(parser, expr);
            break;
        case TOKEN_LBRACE:
        case TOKEN_LESS:
            if (!starts_struct_lit(parser, expr, ctx)) {
                return expr;
            }

            expr = parse_struct_lit_expr(parser, expr);
            break;
        default:
            return expr;
        }
    }

    return NULL;
}

static ASTExpr *parse_unary(Parser *parser, ExprContext ctx) {
    Span span = parser_span(parser);

    if (parser->current.type == TOKEN_BOX) {
        parser_next_token(parser);

        ASTExpr *value = parse_unary(parser, ctx);
        if (!value) {
            return NULL;
        }

        return ast_box_expr_create(parser->arena, span, value);
    }

    if (parser->current.type == TOKEN_MUL || parser->current.type == TOKEN_MINUS ||
        parser->current.type == TOKEN_NOT) {
        TokenType prefix = parser->current.type;

        parser_next_token(parser);

        ASTExpr *target = parse_unary(parser, ctx);
        if (!target) {
            return NULL;
        }

        switch (prefix) {
        case TOKEN_MUL:
            return ast_deref_expr_create(parser->arena, span, target);
        case TOKEN_NOT:
            return ast_not_expr_create(parser->arena, span, target);
        default:
            return ast_neg_expr_create(parser->arena, span, target);
        }
    }

    return parse_postfix(parser, parse_primary(parser), ctx);
}

static ASTExpr *parse_precedence(Parser *parser, int min_precedence, ExprContext ctx) {
    ASTExpr *lhs = parse_unary(parser, ctx);
    if (!lhs)
        return NULL;

    while (1) {
        Token token = parser->current;
        int precedence = get_precedence(token.type);

        if (precedence == 0 || precedence < min_precedence) {
            break;
        }

        parser_next_token(parser);

        BinOp op = parse_bin_op(token.type);
        ASTExpr *rhs = parse_precedence(parser, precedence + 1, ctx);

        if (!rhs) {
            return NULL;
        }

        lhs = ast_bin_op_expr_create(parser->arena, token_span(token), lhs, op, rhs);
    }

    return lhs;
}

static ASTExpr *parse_primary(Parser *parser) {
    Span span = parser_span(parser);
    switch (parser->current.type) {
    case TOKEN_LBRACKET: {
        parser_next_token(parser);

        ASTExprList elements = ast_expr_list_create(arena_allocator(parser->arena));

        while (parser->current.type != TOKEN_RBRACKET) {
            if (parser->current.type == TOKEN_EOF) {
                parser_error(parser, "expected ']' to close the elements");
                return NULL;
            }

            ASTExpr *element = parse_expression(parser, EXPR_ANY);

            if (!element) {
                return NULL;
            }

            ast_expr_list_add(&elements, element);

            if (parser->current.type != TOKEN_COMMA) {
                break;
            }

            parser_next_token(parser);
        }

        if (!parser_expect(parser, TOKEN_RBRACKET, "expected ']' after an array's elements")) {
            return NULL;
        }

        parser_next_token(parser);

        return ast_array_lit_expr_create(parser->arena, span, elements);
    }
    case TOKEN_INT: {
        int32_t value = parser->current.value.as_int;

        parser_next_token(parser);

        return ast_literal_expr_create(parser->arena, span, (Literal){.kind = TYPE_INT, .as_int = value});
    }
    case TOKEN_FLOAT: {
        float value = parser->current.value.as_float;

        parser_next_token(parser);

        return ast_literal_expr_create(parser->arena, span, (Literal){.kind = TYPE_FLOAT, .as_float = value});
    }
    case TOKEN_STRING: {
        String *text = parser->current.value.as_string;

        parser_next_token(parser);

        return ast_literal_expr_create(parser->arena, span, (Literal){.kind = TYPE_STR, .as_string = text});
    }
    case TOKEN_TRUE: {
        parser_next_token(parser);

        return ast_literal_expr_create(parser->arena, span, (Literal){.kind = TYPE_BOOL, .as_int = 1});
    }
    case TOKEN_FALSE: {
        parser_next_token(parser);

        return ast_literal_expr_create(parser->arena, span, (Literal){.kind = TYPE_BOOL, .as_int = 0});
    }
    case TOKEN_IDENT: {
        Token name = parser->current;
        StringRef lexeme = name.lexeme;

        parser_next_token(parser);

        TypeExpr *owner_type_expr = NULL;

        if (parser->current.type == TOKEN_LESS &&
            (parser_type_args_precede_colons(parser) || parser_type_args_precede_a_call(parser))) {
            owner_type_expr = parse_type_args_for(parser, lexeme);

            if (!owner_type_expr) {
                return NULL;
            }
        }

        if (parser->current.type == TOKEN_COLON_COLON) {
            parser_next_token(parser);

            if (!parser_expect(parser, TOKEN_IDENT, "expected a name after '::'")) {
                return NULL;
            }

            StringRef member = parser->current.lexeme;
            lexeme.length = (size_t)(member.data - lexeme.data) + member.length;

            parser_next_token(parser);

            if (parser->current.type == TOKEN_COLON_COLON) {
                parser_error(parser, "a qualified name has one '::', as 'Module::name'");
                return NULL;
            }
        }

        ASTExpr *variable = ast_variable_expr_create(parser->arena, span, lexeme);

        variable->var.owner_type_expr = owner_type_expr;

        return variable;
    }
    case TOKEN_LPAREN: {
        parser_next_token(parser);

        ASTExpr *node = parse_expression(parser, EXPR_ANY);

        if (node == NULL) {
            return NULL;
        }

        if (!parser_expect(parser, TOKEN_RPAREN, "expected ')'")) {
            return NULL;
        }

        parser_next_token(parser);

        return node;
    }
    default:
        parser_error_found(parser, "expected an expression");
        return NULL;
    }
}

static int get_precedence(TokenType type) {
    switch (type) {
    case TOKEN_AND:
    case TOKEN_OR:
        return 1;
    case TOKEN_EQUAL:
    case TOKEN_NEQUAL:
        return 2;
    case TOKEN_LESS:
    case TOKEN_GREATER:
    case TOKEN_LEQUAL:
    case TOKEN_GEQUAL:
        return 3;
    case TOKEN_PLUS:
    case TOKEN_MINUS:
        return 5;
    case TOKEN_MUL:
    case TOKEN_DIV:
    case TOKEN_MOD:
        return 6;
    default:
        return 0;
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
    case TOKEN_MOD:
        return BIN_OP_MOD;
    case TOKEN_AND:
        return BIN_OP_AND;
    case TOKEN_OR:
        return BIN_OP_OR;
    default:
        break;
    }

    assert(0 && "Invalid token type");
    abort();
}

static bool parser_expect(Parser *parser, TokenType token, const char *message) {
    if (parser->current.type != token) {
        diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser), "%s, found %s", message,
                   token_description(parser->current.type));
        return false;
    }

    return true;
}

static void parser_error(Parser *parser, const char *message) {
    diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser), "%s", message);
}

static void parser_error_found(Parser *parser, const char *message) {
    diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser), "%s, found %s", message,
               token_description(parser->current.type));
}
