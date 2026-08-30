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
static ASTStmt *parse_var_decl_stmt(Parser *parser);
static ASTStmt *parse_func_decl_stmt(Parser *parser);
static ASTStmt *parse_struct_decl_stmt(Parser *parser);
static ASTField *parse_field(Parser *parser, const char *name_message);
static TypeExpr *parse_type_expr(Parser *parser);
static ASTExpr *parse_index_expr(Parser *parser, ASTExpr *target);
static ASTExpr *parse_expression(Parser *parser);
static ASTStmt *parse_if_stmt(Parser *parser);
static ASTStmt *parse_for_stmt(Parser *parser);
static ASTStmt *parse_jump_stmt(Parser *parser);
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

// 'module Name;' — the unit's namespace. Required, and before any declaration,
// so this runs once before the declaration loop rather than as one more case
// inside it.
//
// Required because a declaration has to belong to a module that can be named:
// one that could not be would be reachable only by the host, and unreachable
// from any other unit.
static void parse_module_directive(Parser *parser, ASTUnit *unit) {
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

    unit->module_name = name;
    unit->module_span = span;

    if (parser_expect(parser, TOKEN_SEMICOLON, "expected ';' after the module name")) {
        parser_next_token(parser);
    }
}

// 'import Name;' — a module this unit may name. Several may appear, and all of
// them come after the module directive and before any declaration, so they read
// as one block at the top of the unit.
//
// A module is named the same way here as in a qualified reference, so the same
// single-identifier rule applies: 'import A::B;' names nothing this language
// has.
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

        // Reachable only after a declaration, since the first token was handled
        // above: a directive here is either a second one or a late one, and
        // both are the same mistake — it is not where the module is named.
        // Parsed into a throwaway so a misplaced directive cannot change the
        // namespace, but still consumed so it does not cascade.
        if (parser->current.type == TOKEN_IMPORT) {
            diag_error(parser->diagnostics, GAB_ERR_SYNTAX, parser_span(parser),
                       "'import' must appear before any declaration");

            // Parsed into a throwaway so a misplaced import cannot widen what
            // this unit may name, but still consumed so it does not cascade.
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
            // Recover to the next statement boundary so a unit with several
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

        ast_unit_add_statement(unit, stmt);
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
        case TOKEN_EXTERN:
        case TOKEN_STRUCT:
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
        stmt = parse_var_decl_stmt(parser);
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
    default: {
        parser_error_found(parser, "expected a declaration ('let', 'func', 'extern', or 'struct')");
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

    TypeExpr *spec = NULL;
    if (parser->current.type == TOKEN_COLON) {
        parser_next_token(parser); // eat ':'

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

        return ast_var_decl_stmt_create(span, name.lexeme, spec, NULL);
    }

    if (!parser_expect(parser, TOKEN_ASSIGN, "expected ';' or '='")) {
        if (spec) {
            type_expr_destroy(spec);
        }
        return NULL;
    }

    parser_next_token(parser); // eat '='

    ASTExpr *initializer = parse_expression(parser);
    if (!initializer) {
        if (spec) {
            type_expr_destroy(spec);
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

// Parses one clause of the three-clause form: a 'let' or an assignment, never
// a declaration that would escape the loop header. The clause is optional in
// every position, so an immediate terminator yields no statement and no error.
static ASTStmt *parse_for_clause(Parser *parser, TokenType terminator) {
    if (parser->current.type == terminator) {
        return NULL;
    }

    if (parser->current.type == TOKEN_LET) {
        return parse_var_decl_stmt(parser);
    }

    return parse_expr_stmt(parser);
}

// 'for' spells all three loop forms, told apart by what follows the keyword:
// '{' is the infinite loop, a single expression before '{' is the condition
// form, and anything followed by ';' is the three-clause form. An absent
// condition means the same in each form -- loop forever -- so the node holds
// NULL rather than a synthesized 'true'.
//
// A '{' after the condition can only open the body, since the language has no
// struct literal for it to begin instead.
static ASTStmt *parse_for_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser); // eat "for"

    if (parser->current.type == TOKEN_LBRACE) {
        ASTStmt *body = parse_block_stmt(parser);
        if (!body) {
            return NULL;
        }

        return ast_for_stmt_create(span, NULL, NULL, NULL, body);
    }

    ASTStmt *init = NULL;
    ASTExpr *condition = NULL;
    ASTStmt *post = NULL;

    // The condition form and the clause form share this first clause: it is a
    // condition until a ';' proves it was the initializer.
    ASTStmt *first = parse_for_clause(parser, TOKEN_SEMICOLON);

    if (parser->current.type != TOKEN_SEMICOLON) {
        if (!first) {
            parser_error_found(parser, "expected a loop condition or '{'");
            return NULL;
        }

        if (first->kind != STMT_EXPR) {
            parser_error(parser, "a loop condition must be an expression");
            ast_stmt_destroy(first);
            return NULL;
        }

        // Unwrapped from its statement: the condition form has no initializer,
        // and what was parsed as one is really the condition.
        condition = first->expr.value;
        first->expr.value = NULL;
        ast_stmt_destroy(first);

        ASTStmt *body = parse_block_stmt(parser);
        if (!body) {
            ast_expr_free(condition);
            return NULL;
        }

        return ast_for_stmt_create(span, NULL, condition, NULL, body);
    }

    init = first;

    parser_next_token(parser); // eat ';'

    if (parser->current.type != TOKEN_SEMICOLON) {
        condition = parse_expression(parser);
        if (!condition) {
            ast_stmt_destroy(init);
            return NULL;
        }
    }

    if (!parser_expect(parser, TOKEN_SEMICOLON, "expected ';' after the loop condition")) {
        ast_stmt_destroy(init);
        ast_expr_free(condition);
        return NULL;
    }

    parser_next_token(parser); // eat ';'

    post = parse_for_clause(parser, TOKEN_LBRACE);

    ASTStmt *body = parse_block_stmt(parser);
    if (!body) {
        ast_stmt_destroy(init);
        ast_expr_free(condition);
        ast_stmt_destroy(post);
        return NULL;
    }

    return ast_for_stmt_create(span, init, condition, post, body);
}

static ASTStmt *parse_jump_stmt(Parser *parser) {
    Span span = parser_span(parser);
    bool is_break = parser->current.type == TOKEN_BREAK;

    parser_next_token(parser); // eat "break" or "continue"

    return ast_jump_stmt_create(span, is_break);
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

    TypeExpr *type = parse_type_expr(parser);
    if (!type) {
        return NULL;
    }

    return ast_field_create(span, name, type);
}

// A type position: any number of 'box' or 'ref', then a name, then the
// arguments a constructor takes.
//
// Built outward-in, which is the order it reads: each wrapper is made once its
// inner is parsed, so the tree's shape is the spelling's own and nothing counts
// levels or records which of them borrow.
static TypeExpr *parse_type_expr(Parser *parser) {
    // '[T; N]' is a run of N of T. The element is delimited rather than
    // prefixed, so what the length counts is exactly what the brackets hold:
    // '[box Box; 2]' is two pointers, and 'box [Box; 2]' is one pointer to two
    // Boxes. A suffix would have had to answer which of those it meant.
    if (parser->current.type == TOKEN_LBRACKET) {
        parser_next_token(parser); // eat '['

        TypeExpr *element = parse_type_expr(parser);

        if (!element) {
            return NULL;
        }

        if (!parser_expect(parser, TOKEN_SEMICOLON, "expected ';' and a length, as '[int; 3]'")) {
            type_expr_destroy(element);
            return NULL;
        }

        parser_next_token(parser); // eat ';'

        // A literal and only a literal: interning a type on its length means
        // the length has to be known where the type is named.
        if (parser->current.type != TOKEN_INT) {
            parser_error(parser, "an array's length must be an integer literal");
            type_expr_destroy(element);
            return NULL;
        }

        int32_t length = parser->current.value.as_int;

        parser_next_token(parser); // eat the length

        if (!parser_expect(parser, TOKEN_RBRACKET, "expected ']' after an array's length")) {
            type_expr_destroy(element);
            return NULL;
        }

        parser_next_token(parser); // eat ']'

        // Its own node rather than a synthesized name: the language spells this
        // shape, so nothing has to be in scope for it to mean what it means.
        return type_expr_array(element, length);
    }

    // 'ref T' is a borrow: a pointer that does not own what it names. It stands
    // in place of the 'box', not before it -- 'ref T' and 'box T' are both one
    // pointer deep, and differ only in who frees the inner. The two nest in any
    // order, so each is its own wrapper around whatever follows it.
    if (parser->current.type == TOKEN_REF || parser->current.type == TOKEN_BOX) {
        TypeExprKind kind = parser->current.type == TOKEN_REF ? TYPE_EXPR_REF : TYPE_EXPR_BOX;

        parser_next_token(parser); // eat 'ref' or 'box'

        TypeExpr *inner = parse_type_expr(parser);

        if (!inner) {
            return NULL;
        }

        return type_expr_indirect(kind, inner);
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

    TypeExpr *base = type_expr_name(name);

    // A type name followed by '<' applies it to what the delimiters hold:
    // 'Vec<int>', 'Map<K,V>'. One rule for every constructor that takes type
    // arguments, so a generic declaration added later parses here without a
    // rule of its own.
    //
    // Delimited rather than juxtaposed. Where an argument simply followed the
    // name, a type ending one construct and an identifier beginning the next
    // could not be told from an application.
    TypeExpr *type = base;

    if (parser->current.type == TOKEN_LESS) {
        parser_next_token(parser); // eat '<'

        TypeExpr *apply = type_expr_apply(base);

        for (;;) {
            TypeExpr *argument = parse_type_expr(parser);

            if (!argument) {
                type_expr_destroy(apply);
                return NULL;
            }

            type_expr_list_add(&apply->apply.args, argument);

            if (parser->current.type != TOKEN_COMMA) {
                break;
            }

            parser_next_token(parser); // eat ','
        }

        if (!parser_expect(parser, TOKEN_GREATER, "expected '>' after a type's arguments")) {
            type_expr_destroy(apply);
            return NULL;
        }

        parser_next_token(parser); // eat '>'

        type = apply;
    }

    return type;
}

static ASTStmt *parse_struct_decl_stmt(Parser *parser) {
    Span span = parser_span(parser);

    parser_next_token(parser); // eat "struct"

    if (!parser_expect(parser, TOKEN_IDENT, "expected a struct name")) {
        return NULL;
    }

    StringRef name = parser->current.lexeme;
    parser_next_token(parser); // eat struct name

    // The names this declaration takes, spelled where a mention spells its
    // arguments. A struct writing none is the same declaration applied to
    // nothing, so there is no second rule for the plain case.
    StringRef params[GAB_MAX_TYPE_PARAMS];
    size_t param_count = 0;

    if (parser->current.type == TOKEN_LESS) {
        parser_next_token(parser); // eat '<'

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
            parser_next_token(parser); // eat the parameter name

            if (parser->current.type != TOKEN_COMMA) {
                break;
            }

            parser_next_token(parser); // eat ','
        }

        if (!parser_expect(parser, TOKEN_GREATER, "expected '>' after a struct's type parameters")) {
            return NULL;
        }

        parser_next_token(parser); // eat '>'
    }

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

    return ast_struct_decl_stmt_create(span, name, params, param_count, fields);
}

// 'extern func f(x: int): int;' declares a signature whose body the host
// supplies. The keyword is what makes a missing body a declaration rather than
// an error, so the two spellings never have to be told apart by guessing.
static ASTStmt *parse_func_decl_stmt(Parser *parser) {
    Span span = parser_span(parser);

    bool is_extern = parser->current.type == TOKEN_EXTERN;

    if (is_extern) {
        parser_next_token(parser); // eat "extern"

        if (!parser_expect(parser, TOKEN_FUNC, "expected 'func' after 'extern'")) {
            return NULL;
        }
    }

    parser_next_token(parser); // eat "func"

    if (!parser_expect(parser, TOKEN_IDENT, "expected a function name")) {
        return NULL;
    }

    StringRef func_name = parser->current.lexeme;
    parser_next_token(parser); // eat func name

    // 'func Vec::new(...)' declares a function on a type that no value reaches:
    // what was read as the name is the owning type, and the name follows.
    // The type this attaches to, named before '::'.
    TypeExpr *owner = NULL;

    if (parser->current.type == TOKEN_COLON_COLON) {
        parser_next_token(parser); // eat '::'

        if (!parser_expect(parser, TOKEN_IDENT, "expected a function name after '::'")) {
            return NULL;
        }

        owner = type_expr_name(func_name);
        func_name = parser->current.lexeme;

        parser_next_token(parser); // eat the function name

        // One '::', as everywhere else it is written: the owner is a type in
        // this module, so a second has nothing left to qualify.
        if (parser->current.type == TOKEN_COLON_COLON) {
            parser_error(parser, "a function owned by a type has one '::', as 'Type::name'");
            type_expr_destroy(owner);
            return NULL;
        }
    }

    if (!parser_expect(parser, TOKEN_LPAREN, "expected '(' after function name")) {
        return NULL;
    }

    parser_next_token(parser); // eat '(';

    ASTFieldList func_params = ast_field_list_create();
    while (parser->current.type != TOKEN_RPAREN) {
        ASTField *param = parse_field(parser, "expected a parameter name");
        if (!param) {
            ast_field_list_free(&func_params);
            return NULL;
        }

        if (parser->current.type != TOKEN_COMMA && parser->current.type != TOKEN_RPAREN) {
            parser_error_found(parser, "expected ',' or ')' after parameter");
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

    TypeExpr *func_type = NULL;
    if (parser->current.type == TOKEN_COLON) {
        parser_next_token(parser); // eat ':'

        func_type = parse_type_expr(parser);
        if (!func_type) {
            ast_field_list_free(&func_params);
            return NULL;
        }
    }

    // An extern states a signature and stops there; the ';' is consumed by
    // parse_decl_statement, which stmt_needs_terminator sends it to.
    if (is_extern) {
        if (parser->current.type == TOKEN_LBRACE) {
            parser_error(parser, "an 'extern' function is defined by the host and cannot have a body");

            type_expr_destroy(func_type);
            ast_field_list_free(&func_params);
            ast_stmt_destroy(parse_block_stmt(parser));

            return NULL;
        }

        ASTStmt *decl = ast_func_decl_stmt_create(span, func_name, func_type, func_params, NULL);
        decl->func_decl.owner = owner;

        return decl;
    }

    ASTStmt *func_body = parse_block_stmt(parser);
    if (!func_body) {
        // The return type is parsed before the body, so a body that fails to
        // parse leaves it owned by nobody: the node that would have taken it is
        // never created. The receiver is in the same position.
        type_expr_destroy(func_type);
        ast_field_list_free(&func_params);

        return NULL;
    }

    ASTStmt *decl = ast_func_decl_stmt_create(span, func_name, func_type, func_params, func_body);
    decl->func_decl.owner = owner;

    return decl;
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

// Whether a token is a compound assignment operator, and which binary
// operation it assigns the result of. 'op' may be NULL to ask only the former.
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

static ASTStmt *parse_expr_stmt(Parser *parser) {
    Span span = parser_span(parser);

    ASTExpr *expr = parse_expression(parser);
    if (expr == NULL) {
        return NULL;
    }

    bool compound = compound_assign_op(parser->current.type, NULL);

    if (parser->current.type != TOKEN_ASSIGN && !compound) {
        return ast_expr_stmt_create(span, expr);
    }

    if (expr->kind != EXPR_VARIABLE && expr->kind != EXPR_FIELD && expr->kind != EXPR_DEREF &&
        expr->kind != EXPR_INDEX) {
        parser_error(parser, "expression is not assignable");
        ast_expr_free(expr);
        return NULL;
    }

    BinOp op = BIN_OP_ADD;
    compound_assign_op(parser->current.type, &op);

    parser_next_token(parser); // eat '=' or a compound assignment operator

    ASTExpr *value = parse_expression(parser);
    if (!value) {
        ast_expr_free(expr);
        return NULL;
    }

    // The target is kept as one expression rather than copied onto both sides
    // of an expansion, so that whatever it names -- a call included -- is
    // evaluated once. Codegen is what reads it and writes it back.
    if (compound) {
        return ast_compound_assign_stmt_create(span, expr, op, value);
    }

    return ast_assign_stmt_create(span, expr, value);
}

static bool stmt_needs_terminator(ASTStmt *stmt) {
    switch (stmt->kind) {
    case STMT_IF:
    case STMT_FOR:
    case STMT_BLOCK:
    case STMT_STRUCT_DECL:
        return false;
    // A body closes itself with '}'. An extern has none, so it ends the way
    // every other bodyless declaration does.
    case STMT_FUNC_DECL:
        return stmt->func_decl.body == NULL;
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

// 'xs[i]'. A postfix like a call, so it chains: 'a.b[i].c' and 'xs[i][j]' both
// fall out of the loop that reads it.
static ASTExpr *parse_index_expr(Parser *parser, ASTExpr *target) {
    Span span = parser_span(parser);

    parser_next_token(parser); // eat '['

    ASTExpr *index = parse_expression(parser);

    if (!index) {
        ast_expr_free(target);
        return NULL;
    }

    if (!parser_expect(parser, TOKEN_RBRACKET, "expected ']' after an index")) {
        ast_expr_free(target);
        ast_expr_free(index);
        return NULL;
    }

    parser_next_token(parser); // eat ']'

    return ast_index_expr_create(span, target, index);
}

// A primary with its postfixes, or a prefix form over one of those. Prefix '&'
// and '*' bind tighter than any binary operator but looser than a postfix, so
// 'ref p.x' takes the address of the field and '*p.x' dereferences it.
static ASTExpr *parse_unary(Parser *parser) {
    Span span = parser_span(parser);

    // 'new T' names a type rather than taking an operand, so it is handled
    // before the operand-taking prefixes below.
    if (parser->current.type == TOKEN_NEW) {
        parser_next_token(parser); // eat 'new'

        TypeExpr *spec = parse_type_expr(parser);
        if (!spec) {
            return NULL;
        }

        return ast_new_expr_create(span, spec);
    }

    // '[a, b, c]' writes out an array's elements. A prefix '[' is unambiguous:
    // the postfix one indexes, and that only ever follows an operand.
    if (parser->current.type == TOKEN_LBRACKET) {
        parser_next_token(parser); // eat '['

        ASTExprList elements = ast_expr_list_create();

        while (parser->current.type != TOKEN_RBRACKET) {
            if (parser->current.type == TOKEN_EOF) {
                parser_error(parser, "expected ']' to close the elements");
                ast_expr_list_free(&elements);
                return NULL;
            }

            ASTExpr *element = parse_expression(parser);

            if (!element) {
                ast_expr_list_free(&elements);
                return NULL;
            }

            ast_expr_list_add(&elements, element);

            if (parser->current.type != TOKEN_COMMA) {
                break;
            }

            parser_next_token(parser); // eat ','
        }

        if (!parser_expect(parser, TOKEN_RBRACKET, "expected ']' after an array's elements")) {
            ast_expr_list_free(&elements);
            return NULL;
        }

        parser_next_token(parser); // eat ']'

        return ast_array_lit_expr_create(span, elements);
    }

    // Some of these share a token with a binary operator -- '*' with
    // multiplication, '-' with subtraction -- and only its position tells the
    // two apart. Recursing into parse_unary is what makes them stack, so
    // '--x' and '-*p' both parse.
    if (parser->current.type == TOKEN_MUL || parser->current.type == TOKEN_MINUS ||
        parser->current.type == TOKEN_NOT) {
        TokenType prefix = parser->current.type;

        parser_next_token(parser); // eat '*', '-' or '!'

        ASTExpr *target = parse_unary(parser);
        if (!target) {
            return NULL;
        }

        switch (prefix) {
        case TOKEN_MUL:
            return ast_deref_expr_create(span, target);
        case TOKEN_NOT:
            return ast_not_expr_create(span, target);
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
    while (parser->current.type == TOKEN_LPAREN || parser->current.type == TOKEN_DOT ||
           parser->current.type == TOKEN_LBRACKET) {
        if (parser->current.type == TOKEN_LPAREN) {
            expr = parse_call_expr(parser, expr);
        } else if (parser->current.type == TOKEN_LBRACKET) {
            expr = parse_index_expr(parser, expr);
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
        int32_t value = parser->current.value.as_int;

        parser_next_token(parser); // eat integer

        return ast_literal_expr_create(span, (Literal){.kind = TYPE_INT, .as_int = value});
    }
    case TOKEN_FLOAT: {
        float value = parser->current.value.as_float;

        parser_next_token(parser); // eat float

        return ast_literal_expr_create(span, (Literal){.kind = TYPE_FLOAT, .as_float = value});
    }
    case TOKEN_STRING: {
        String *text = parser->current.value.as_string;

        parser_next_token(parser); // eat string

        return ast_literal_expr_create(span, (Literal){.kind = TYPE_STR, .as_string = text});
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
        StringRef lexeme = name.lexeme;

        parser_next_token(parser); // eat identifier

        // 'Module::name' and 'Type::name' are one qualified name, kept as one
        // ref over the source the way a qualified type name is: which of the
        // two the first half names is the resolver's question, not the
        // grammar's.
        if (parser->current.type == TOKEN_COLON_COLON) {
            parser_next_token(parser); // eat '::'

            if (!parser_expect(parser, TOKEN_IDENT, "expected a name after '::'")) {
                return NULL;
            }

            StringRef member = parser->current.lexeme;
            lexeme.length = (size_t)(member.data - lexeme.data) + member.length;

            parser_next_token(parser); // eat the member name

            if (parser->current.type == TOKEN_COLON_COLON) {
                parser_error(parser, "a qualified name has one '::', as 'Module::name'");
                return NULL;
            }
        }

        return ast_variable_expr_create(span, lexeme);
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
        return 5;
    case TOKEN_MUL:
    case TOKEN_DIV:
    case TOKEN_MOD:
        return 6;
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
