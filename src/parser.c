#include "parser.h"
#include "ast/ast.h"
#include "ast/stmt.h"
#include "lexer.h"
#include "string/string_ref.h"
#include "type.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static ASTStmt *parse_decl_statement(Parser *parser);
static ASTStmt *parse_statement(Parser *parser);
static ASTStmt *parse_var_decl_stmt(Parser *parser);
static ASTStmt *parse_func_decl_stmt(Parser *parser);
static ASTStmt *parse_struct_decl_stmt(Parser *parser);
static ASTField *parse_field(Parser *parser, const char *name_message);
static TypeSpec *parse_type_spec(Parser *parser);
static ASTStmt *parse_if_stmt(Parser *parser);
static ASTStmt *parse_block_stmt(Parser *parser);
static ASTStmt *parse_return_stmt(Parser *parser);
static ASTStmt *parse_expr_stmt(Parser *parser);
static bool stmt_needs_terminator(ASTStmt *stmt);

static ASTExpr *parse_expression(Parser *parser);
static ASTExpr *parse_primary(Parser *parser);
static ASTExpr *parse_unary(Parser *parser);
static ASTExpr *parse_field_expr(Parser *parser, ASTExpr *target);
static ASTExpr *parse_method_call_expr(Parser *parser, ASTExpr *receiver, StringRef name, Span span);
static void parser_synchronize(Parser *parser);
static ASTExpr *parse_precedence(Parser *parser, int min_precedence);
static int get_precedence(TokenType type);
static BinOp parse_bin_op(TokenType type);

static bool parser_expect(Parser *parser, TokenType token, const char *message);
static void parser_error_found(Parser *parser, const char *message);
static void parser_error(Parser *parser, const char *message);

Parser parser_create(Lexer *lexer, Diagnostics *diagnostics) {
    return (Parser){
        .lexer = lexer,
        .diagnostics = diagnostics,
    };
}

void parser_next_token(Parser *parser) { parser->current = lexer_next(parser->lexer); }

static Span parser_span(Parser *parser) { return token_span(parser->current); }

// 'module Name;' — the unit's namespace. Optional, but if present it must come
// before any declaration, so this runs once before the declaration loop rather
// than as one more case inside it.
static void parse_module_directive(Parser *parser, ASTScript *script) {
    Span span = parser_span(parser);

    parser_next_token(parser);

    if (!parser_expect(parser, TOKEN_IDENT, "expected a module name after 'module'")) {
        return;
    }

    StringRef name = parser->current.lexeme;

    parser_next_token(parser);

    // A nested name has to mean either a hierarchy or a longer flat name, and
    // both readings are worse than an error until something needs one. Rejecting
    // it keeps a qualified name unambiguous: one '::', module then symbol.
    if (parser->current.type == TOKEN_COLON_COLON) {
        // The whole attempted name, so the message shows what was written
        // rather than just the first segment.
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

    script->module_name = name;
    script->module_span = span;

    if (parser_expect(parser, TOKEN_SEMICOLON, "expected ';' after the module name")) {
        parser_next_token(parser);
    }
}

bool parser_parse(Parser *parser, ASTScript *script) {
    size_t errors_before = diagnostics_count(parser->diagnostics);

    parser_next_token(parser);

    if (parser->current.type == TOKEN_MODULE) {
        parse_module_directive(parser, script);
    }

    while (parser->current.type != TOKEN_EOF) {
        if (parser->current.type == TOKEN_SEMICOLON) {
            parser_next_token(parser);
            continue;
        }

        // Reachable only after a declaration, since the first token was handled
        // above: a directive here is either a second one or a late one, and
        // both are the same mistake — it is not where the module is named.
        // Parsed into a throwaway so a misplaced directive cannot change the
        // namespace, but still consumed so it does not cascade.
        if (parser->current.type == TOKEN_MODULE) {
            diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser),
                       "'module' must appear once, before any declaration");

            ASTScript discarded = *script;
            parse_module_directive(parser, &discarded);
            continue;
        }

        ASTStmt *stmt = parse_decl_statement(parser);
        if (!stmt) {
            // Recover to the next statement boundary so a script with several
            // syntax errors reports all of them rather than just the first.
            TokenType before = parser->current.type;
            int before_pos = parser->lexer->pos;

            parser_synchronize(parser);

            // Synchronizing stops at '}' without consuming it, which at the top
            // level would leave us stuck on a stray brace. Always make progress.
            if (parser->current.type == before && parser->lexer->pos == before_pos) {
                parser_next_token(parser);
            }

            continue;
        }

        ast_script_add_statement(script, stmt);
    }

    return diagnostics_count(parser->diagnostics) == errors_before;
}

// Skip tokens until just past a statement boundary, so parsing can resume at
// something that plausibly starts a new statement.
static void parser_synchronize(Parser *parser) {
    while (parser->current.type != TOKEN_EOF) {
        if (parser->current.type == TOKEN_SEMICOLON) {
            parser_next_token(parser);
            return;
        }

        // Leave '}' for the enclosing block to consume; eating it here would
        // strand that block and produce a spurious "expected '}'".
        if (parser->current.type == TOKEN_RBRACE) {
            return;
        }

        switch (parser->current.type) {
        case TOKEN_LET:
        case TOKEN_FUNC:
        case TOKEN_STRUCT:
        case TOKEN_MODULE:
        case TOKEN_IF:
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
        stmt = parse_var_decl_stmt(parser);
        break;
    }
    case TOKEN_FUNC: {
        stmt = parse_func_decl_stmt(parser);
        break;
    }
    case TOKEN_STRUCT: {
        stmt = parse_struct_decl_stmt(parser);
        break;
    }
    default: {
        parser_error_found(parser, "expected a declaration ('let', 'func', or 'struct')");
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
        ast_stmt_destroy(stmt);
        return NULL;
    }

    parser_next_token(parser);

    return stmt;
}

static ASTStmt *parse_statement(Parser *parser) {
    ASTStmt *stmt = NULL;

    switch (parser->current.type) {
    case TOKEN_LET: {
        stmt = parse_var_decl_stmt(parser);
        break;
    }
    case TOKEN_FUNC: {
        // Reserved rather than merely unimplemented. A nested function today
        // could not capture anything, so it would be a free function with a
        // narrower name — and once closures exist this same syntax has to mean
        // a capturing one. Allowing the weaker meaning now would silently
        // change what already-written code means later, so the syntax is kept
        // unclaimed until closures can define it.
        parser_error(parser, "a function cannot be declared inside another; declare it at module level");

        // Consumed anyway, so the body does not cascade into a second error at
        // every statement it contains.
        ast_stmt_destroy(parse_func_decl_stmt(parser));

        return NULL;
    }
    case TOKEN_STRUCT: {
        stmt = parse_struct_decl_stmt(parser);
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

    if (!parser_expect(parser, TOKEN_SEMICOLON, "expected ';'")) {
        ast_stmt_destroy(stmt);
        return NULL;
    }

    parser_next_token(parser);

    return stmt;
}

static ASTStmt *parse_var_decl_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser); // eat "let"

    if (!parser_expect(parser, TOKEN_IDENT, "expected a variable name after 'let'")) {
        return NULL;
    }

    Token name = parser->current;

    parser_next_token(parser); // eat identifier

    TypeSpec *spec = NULL;
    if (parser->current.type == TOKEN_COLON) {
        parser_next_token(parser); // eat ':'

        spec = parse_type_spec(parser);
        if (!spec) {
            return NULL;
        }
    }

    if (parser->current.type == TOKEN_SEMICOLON) {
        if (!spec) {
            parser_error(parser, "expected a type or an initializer");
            return NULL;
        }

        return ast_var_decl_stmt_create(span, name.lexeme, spec, NULL);
    }

    if (!parser_expect(parser, TOKEN_ASSIGN, "expected ';' or '='")) {
        if (spec) {
            type_spec_destroy(spec);
        }
        return NULL;
    }

    parser_next_token(parser); // eat '='

    ASTExpr *initializer = parse_expression(parser);
    if (!initializer) {
        if (spec) {
            type_spec_destroy(spec);
        }
        return NULL;
    }

    return ast_var_decl_stmt_create(span, name.lexeme, spec, initializer);
}

static ASTStmt *parse_if_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser); // eat "if"

    ASTExpr *condition = parse_expression(parser);
    if (!condition) {
        return NULL;
    }

    ASTStmt *then_block = parse_block_stmt(parser);
    if (!then_block) {
        ast_expr_free(condition);
        return NULL;
    }

    if (parser->current.type != TOKEN_ELSE) {
        return ast_if_stmt_create(span, condition, then_block, NULL);
    }

    parser_next_token(parser); // eat "else"
    ASTStmt *else_block = parse_block_stmt(parser);
    if (!else_block) {
        ast_expr_free(condition);
        ast_stmt_destroy(then_block);
        return NULL;
    }

    return ast_if_stmt_create(span, condition, then_block, else_block);
}

static ASTStmt *parse_block_stmt(Parser *parser) {
    Span span = parser_span(parser);

    if (!parser_expect(parser, TOKEN_LBRACE, "expected '{'")) {
        return NULL;
    }

    parser_next_token(parser); // eat '{'

    ASTStmtList list = ast_stmt_list_create();
    while (parser->current.type != TOKEN_RBRACE) {
        if (parser->current.type == TOKEN_EOF) {
            parser_error_found(parser, "expected '}' to close the block");
            ast_stmt_list_free(&list);
            return NULL;
        }

        ASTStmt *stmt = parse_statement(parser);

        if (!stmt) {
            // Recover within the block so later statements still get checked.
            parser_synchronize(parser);
            continue;
        }

        ast_stmt_list_add(&list, stmt);
    }

    parser_next_token(parser); // eat '}'

    return ast_block_stmt_create(span, list);
}

static ASTField *parse_field(Parser *parser, const char *name_message) {
    if (!parser_expect(parser, TOKEN_IDENT, name_message)) {
        return NULL;
    }

    Span span = parser_span(parser);
    StringRef name = parser->current.lexeme;
    parser_next_token(parser); // eat name

    if (!parser_expect(parser, TOKEN_COLON, "expected ':' after name")) {
        return NULL;
    }

    parser_next_token(parser); // eat ':'

    TypeSpec *type = parse_type_spec(parser);
    if (!type) {
        return NULL;
    }

    return ast_field_create(span, name, type);
}

// A type position, which is a name preceded by any number of '*'. Prefix
// '*' is the same token as multiplication; position is what tells them apart.
static TypeSpec *parse_type_spec(Parser *parser) {
    unsigned int pointer_depth = 0;

    // 'ref T' is a borrow: a pointer that does not own what it names. It stands
    // in place of the '*', not before it — 'ref T' and '*T' are both one
    // pointer deep, and differ only in who frees the pointee.
    // 'ref T' is a borrow, and 'ref ref T' a borrow of one — which is what '&'
    // applied twice produces. Each 'ref' is one level of pointer, so they count
    // the same way stars do.
    bool is_ref = parser->current.type == TOKEN_REF;

    while (parser->current.type == TOKEN_REF) {
        pointer_depth++;
        parser_next_token(parser); // eat 'ref'
    }

    // Every level is a borrow or none is: mixing them would need a flag per
    // level, and nothing yet wants '*ref T' — an owning pointer to a borrow.
    if (is_ref && parser->current.type == TOKEN_MUL) {
        parser_error(parser, "'ref' does not combine with '*', as 'ref T' or 'ref ref T'");
        return NULL;
    }

    while (parser->current.type == TOKEN_MUL) {
        pointer_depth++;
        parser_next_token(parser); // eat '*'
    }

    if (!parser_expect(parser, TOKEN_IDENT, "expected a type")) {
        return NULL;
    }

    StringRef name = parser->current.lexeme;
    parser_next_token(parser); // eat the type name

    // 'Module::Type' names a type in another module. The whole thing is kept
    // as one ref over the source, so the resolver sees the qualified name
    // exactly as the registry stores it and needs no rejoining.
    if (parser->current.type == TOKEN_COLON_COLON) {
        parser_next_token(parser); // eat '::'

        if (!parser_expect(parser, TOKEN_IDENT, "expected a type name after '::'")) {
            return NULL;
        }

        StringRef member = parser->current.lexeme;
        name.length = (size_t)(member.data - name.data) + member.length;

        parser_next_token(parser); // eat the type name

        // One '::' only: a module name is a single identifier, so a second
        // would have nothing left to qualify.
        if (parser->current.type == TOKEN_COLON_COLON) {
            parser_error(parser, "a qualified type name has one '::', as 'Module::Type'");
            return NULL;
        }
    }

    return type_spec_create(name, pointer_depth, is_ref);
}

static ASTStmt *parse_struct_decl_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser); // eat "struct"

    if (!parser_expect(parser, TOKEN_IDENT, "expected a struct name")) {
        return NULL;
    }

    StringRef name = parser->current.lexeme;
    parser_next_token(parser); // eat struct name

    if (!parser_expect(parser, TOKEN_LBRACE, "expected '{' after struct name")) {
        return NULL;
    }

    parser_next_token(parser); // eat '{'

    ASTFieldList fields = ast_field_list_create();
    while (parser->current.type != TOKEN_RBRACE) {
        if (parser->current.type == TOKEN_EOF) {
            parser_error(parser, "expected '}' to close the struct");
            ast_field_list_free(&fields);
            return NULL;
        }

        ASTField *field = parse_field(parser, "expected a field name");
        if (!field) {
            ast_field_list_free(&fields);
            return NULL;
        }

        ast_field_list_add(&fields, field);

        if (parser->current.type != TOKEN_COMMA && parser->current.type != TOKEN_RBRACE) {
            parser_error_found(parser, "expected ',' or '}' after field");
            ast_field_list_free(&fields);
            return NULL;
        }

        if (parser->current.type == TOKEN_COMMA) {
            parser_next_token(parser); // eat ','
        }
    }

    parser_next_token(parser); // eat '}'

    return ast_struct_decl_stmt_create(span, name, fields);
}

static ASTStmt *parse_func_decl_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser); // eat "func"

    // 'func (p: *Player) damage(...)' — an optional receiver clause makes this
    // a method on that type rather than a free function. Written with a colon
    // like every other binding in the language, unlike Go's 'p *Player'.
    ASTField *receiver = NULL;
    if (parser->current.type == TOKEN_LPAREN) {
        parser_next_token(parser); // eat '('

        receiver = parse_field(parser, "expected a receiver name");
        if (!receiver) {
            return NULL;
        }

        if (!parser_expect(parser, TOKEN_RPAREN, "expected ')' after the receiver")) {
            ast_field_destroy(receiver);
            return NULL;
        }

        parser_next_token(parser); // eat ')'
    }

    if (!parser_expect(parser, TOKEN_IDENT, "expected a function name")) {
        ast_field_destroy(receiver);
        return NULL;
    }

    StringRef func_name = parser->current.lexeme;
    parser_next_token(parser); // eat func name

    if (!parser_expect(parser, TOKEN_LPAREN, "expected '(' after function name")) {
        ast_field_destroy(receiver);
        return NULL;
    }

    parser_next_token(parser); // eat '(';

    ASTFieldList func_params = ast_field_list_create();
    while (parser->current.type != TOKEN_RPAREN) {
        ASTField *param = parse_field(parser, "expected a parameter name");
        if (!param) {
            ast_field_destroy(receiver);
            ast_field_list_free(&func_params);
            return NULL;
        }

        if (parser->current.type != TOKEN_COMMA && parser->current.type != TOKEN_RPAREN) {
            parser_error_found(parser, "expected ',' or ')' after parameter");
            ast_field_destroy(receiver);
            ast_field_destroy(param);
            ast_field_list_free(&func_params);
            return NULL;
        }

        if (parser->current.type == TOKEN_COMMA) {
            parser_next_token(parser); // eat ','
        }

        ast_field_list_add(&func_params, param);
    }

    parser_next_token(parser); // eat ')'

    TypeSpec *func_type = NULL;
    if (parser->current.type == TOKEN_COLON) {
        parser_next_token(parser); // eat ':'

        func_type = parse_type_spec(parser);
        if (!func_type) {
            ast_field_destroy(receiver);
            ast_field_list_free(&func_params);
            return NULL;
        }
    }

    ASTStmt *func_body = parse_block_stmt(parser);
    if (!func_body) {
        // The return type is parsed before the body, so a body that fails to
        // parse leaves it owned by nobody: the node that would have taken it is
        // never created. The receiver is in the same position.
        ast_field_destroy(receiver);
        type_spec_destroy(func_type);
        ast_field_list_free(&func_params);

        return NULL;
    }

    return ast_func_decl_stmt_create(span, func_name, receiver, func_type, func_params, func_body);
}

static ASTStmt *parse_return_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser); // eat "return"

    ASTExpr *result = parse_expression(parser);
    if (!result) {
        return NULL;
    }

    return ast_return_stmt_create(span, result);
}

static ASTStmt *parse_expr_stmt(Parser *parser) {
    Span span = parser_span(parser);

    ASTExpr *expr = parse_expression(parser);
    if (expr == NULL) {
        return NULL;
    }

    if (parser->current.type != TOKEN_ASSIGN) {
        return ast_expr_stmt_create(span, expr);
    }

    if (expr->kind != EXPR_VARIABLE && expr->kind != EXPR_FIELD && expr->kind != EXPR_DEREF) {
        parser_error(parser, "expression is not assignable");
        ast_expr_free(expr);
        return NULL;
    }

    parser_next_token(parser); // eat '='

    ASTExpr *value = parse_expression(parser);
    if (!value) {
        ast_expr_free(expr);
        return NULL;
    }

    return ast_assign_stmt_create(span, expr, value);
}

static bool stmt_needs_terminator(ASTStmt *stmt) {
    switch (stmt->kind) {
    case STMT_IF:
    case STMT_BLOCK:
    case STMT_FUNC_DECL:
    case STMT_STRUCT_DECL:
        return false;
    default:
        return true;
    }
}

static ASTExpr *parse_expression(Parser *parser) { return parse_precedence(parser, 0); }

static void ast_expr_list_destroy(ASTExprList *list) {
    for (size_t i = 0; i < list->size; i++) {
        ast_expr_free(list->data[i]);
    }

    ast_expr_list_free(list);
}

// Parses the argument list of a call, with the '(' already current.
static ASTExpr *parse_field_expr(Parser *parser, ASTExpr *target) {
    Span span = parser_span(parser);

    parser_next_token(parser); // eat '.'

    if (!parser_expect(parser, TOKEN_IDENT, "expected a field name after '.'")) {
        ast_expr_free(target);
        return NULL;
    }

    Token name = parser->current;

    parser_next_token(parser); // eat identifier

    // A '.name' followed by '(' is a method call, not a field holding a
    // function: Gab has no function values, so the two can never be confused.
    if (parser->current.type == TOKEN_LPAREN) {
        return parse_method_call_expr(parser, target, name.lexeme, span);
    }

    return ast_field_expr_create(span, target, name.lexeme);
}

// The comma-separated arguments between '(' and ')', with the '(' already
// current and consumed here. Shared by a plain call and a method call, which
// differ only in what they do with the list.
static bool parse_call_args(Parser *parser, ASTExprList *out) {
    parser_next_token(parser); // eat '('

    ASTExprList args = ast_expr_list_create();
    bool ok = true;

    while (ok && parser->current.type != TOKEN_RPAREN) {
        if (parser->current.type == TOKEN_EOF) {
            parser_error(parser, "expected ')' to close the argument list");
            ok = false;
            break;
        }

        ASTExpr *arg = parse_expression(parser);
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
            parser_next_token(parser); // eat ','
        }
    }

    if (!ok) {
        ast_expr_list_destroy(&args);
        return false;
    }

    parser_next_token(parser); // eat ')'

    *out = args;
    return true;
}

static ASTExpr *parse_call_expr(Parser *parser, ASTExpr *target) {
    Span span = parser_span(parser);

    ASTExprList args;
    if (!parse_call_args(parser, &args)) {
        ast_expr_free(target);
        return NULL;
    }

    return ast_call_expr_create(span, target, args);
}

// 'recv.name(args)'. Collapsed here rather than left as a call over a field
// access, because this is where the difference between the two is cheapest to
// see: a '.name' followed by '(' is a method call and nothing else.
// 'recv.name(args)' parses as a call whose target is the field expression
// 'recv.name'. There is no separate method-call node: a method is a function
// whose parameter zero is the receiver, and resolution is where that becomes
// true of the tree — it rewrites this into a call with the receiver as argument
// zero. Until then the receiver is simply the field target, which is where the
// parser already put it.
static ASTExpr *parse_method_call_expr(Parser *parser, ASTExpr *receiver, StringRef name, Span span) {
    ASTExpr *target = ast_field_expr_create(span, receiver, name);

    ASTExprList args;
    if (!parse_call_args(parser, &args)) {
        ast_expr_free(target);
        return NULL;
    }

    return ast_call_expr_create(span, target, args);
}

// A primary with its postfixes, or a prefix form over one of those. Prefix '&'
// and '*' bind tighter than any binary operator but looser than a postfix, so
// '&p.x' takes the address of the field and '*p.x' dereferences it.
static ASTExpr *parse_unary(Parser *parser) {
    Span span = parser_span(parser);

    // 'new T' names a type rather than taking an operand, so it is handled
    // before the operand-taking prefixes below.
    if (parser->current.type == TOKEN_NEW) {
        parser_next_token(parser); // eat 'new'

        TypeSpec *spec = parse_type_spec(parser);
        if (!spec) {
            return NULL;
        }

        return ast_new_expr_create(span, spec);
    }

    // Each of these shares its token with a binary operator -- '*' with
    // multiplication, '-' with subtraction -- and only its position tells the
    // two apart. Recursing into parse_unary is what makes them stack, so
    // '--x' and '-*p' both parse.
    if (parser->current.type == TOKEN_AMP || parser->current.type == TOKEN_MUL ||
        parser->current.type == TOKEN_MINUS) {
        TokenType prefix = parser->current.type;

        parser_next_token(parser); // eat '&', '*' or '-'

        ASTExpr *target = parse_unary(parser);
        if (!target) {
            return NULL;
        }

        switch (prefix) {
        case TOKEN_AMP:
            return ast_addr_of_expr_create(span, target);
        case TOKEN_MUL:
            return ast_deref_expr_create(span, target);
        default:
            return ast_neg_expr_create(span, target);
        }
    }

    ASTExpr *expr = parse_primary(parser);
    if (!expr) {
        return NULL;
    }

    // Postfixes bind tighter than any binary operator and chain, so 'a.b.c' and
    // 'f().x' both fall out of looping here.
    while (parser->current.type == TOKEN_LPAREN || parser->current.type == TOKEN_DOT) {
        if (parser->current.type == TOKEN_LPAREN) {
            expr = parse_call_expr(parser, expr);
        } else {
            expr = parse_field_expr(parser, expr);
        }

        if (!expr) {
            return NULL;
        }
    }

    return expr;
}

static ASTExpr *parse_precedence(Parser *parser, int min_precedence) {
    ASTExpr *lhs = parse_unary(parser);
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

        lhs = ast_bin_op_expr_create(token_span(token), lhs, op, rhs);
    }

    return lhs;
}

static ASTExpr *parse_primary(Parser *parser) {
    Span span = parser_span(parser);

    switch (parser->current.type) {
    case TOKEN_INT: {
        char *temp = string_ref_to_cstr(parser->current.lexeme);
        long value = strtol(temp, NULL, 10);
        free(temp);

        parser_next_token(parser); // eat integer

        Literal lit = {.kind = TYPE_INT, .as_int = value};
        return ast_literal_expr_create(span, lit);
    }
    case TOKEN_FLOAT: {
        char *temp = string_ref_to_cstr(parser->current.lexeme);
        double value = strtod(temp, NULL);
        free(temp);

        parser_next_token(parser); // eat float

        Literal lit = {.kind = TYPE_FLOAT, .as_float = value};
        return ast_literal_expr_create(span, lit);
    }
    case TOKEN_TRUE: {
        parser_next_token(parser);

        return ast_literal_expr_create(span, (Literal){.kind = TYPE_BOOL, .as_int = 1});
    }
    case TOKEN_FALSE: {
        parser_next_token(parser);

        return ast_literal_expr_create(span, (Literal){.kind = TYPE_BOOL, .as_int = 0});
    }
    case TOKEN_IDENT: {
        Token name = parser->current;

        parser_next_token(parser); // eat identifier

        return ast_variable_expr_create(span, name.lexeme);
    }
    case TOKEN_LPAREN: {
        parser_next_token(parser); // eat '('

        ASTExpr *node = parse_expression(parser);

        if (node == NULL) {
            return NULL;
        }

        if (!parser_expect(parser, TOKEN_RPAREN, "expected ')'")) {
            ast_expr_free(node);
            return NULL;
        }

        parser_next_token(parser); // eat ')'

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
        return 4;
    case TOKEN_MUL:
    case TOKEN_DIV:
        return 5;
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
        // The caller says what was expected; only the parser knows what is
        // actually there, so it supplies the second half.
        diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser), "%s, found %s", message,
                   token_description(parser->current.type));
        return false;
    }

    return true;
}

static void parser_error(Parser *parser, const char *message) {
    diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser), "%s", message);
}

// For the cases that reject the current token without a specific expected type,
// so they read the same as parser_expect's.
static void parser_error_found(Parser *parser, const char *message) {
    diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser), "%s, found %s", message,
               token_description(parser->current.type));
}
