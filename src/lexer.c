#include "lexer.h"
#include "string/string.h"

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

char lexer_peek(Lexer *lexer) { return lexer->source[lexer->pos]; }

void lexer_eat(Lexer *lexer) {
    if (lexer->source[lexer->pos] == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }

    lexer->pos++;
}

Token token_create(Lexer *lexer, TokenType type) {
    return (Token){.type = type, .line = lexer->start_line, .column = lexer->start_column};
}

Token token_create_ref(Lexer *lexer, TokenType type, StringRef ref) {
    return (Token){.type = type, .lexeme = ref, .line = lexer->start_line, .column = lexer->start_column};
}

Span token_span(Token token) { return (Span){.line = token.line, .column = token.column}; }

// How a token is named in a diagnostic. Punctuation and keywords are quoted so
// they read as source text; the rest get a descriptive noun.
const char *token_description(TokenType type) {
    switch (type) {
    case TOKEN_INVALID:
        return "invalid token";
    case TOKEN_EOF:
        return "end of input";
    case TOKEN_INT:
        return "an integer literal";
    case TOKEN_FLOAT:
        return "a float literal";
    case TOKEN_IDENT:
        return "an identifier";
    case TOKEN_PLUS:
        return "'+'";
    case TOKEN_MINUS:
        return "'-'";
    case TOKEN_MUL:
        return "'*'";
    case TOKEN_DIV:
        return "'/'";
    case TOKEN_ASSIGN:
        return "'='";
    case TOKEN_NOT:
        return "'!'";
    case TOKEN_LESS:
        return "'<'";
    case TOKEN_GREATER:
        return "'>'";
    case TOKEN_EQUAL:
        return "'=='";
    case TOKEN_NEQUAL:
        return "'!='";
    case TOKEN_LEQUAL:
        return "'<='";
    case TOKEN_GEQUAL:
        return "'>='";
    case TOKEN_AND:
        return "'&&'";
    case TOKEN_OR:
        return "'||'";
    case TOKEN_LPAREN:
        return "'('";
    case TOKEN_RPAREN:
        return "')'";
    case TOKEN_LBRACE:
        return "'{'";
    case TOKEN_RBRACE:
        return "'}'";
    case TOKEN_SEMICOLON:
        return "';'";
    case TOKEN_COLON:
        return "':'";
    case TOKEN_COMMA:
        return "','";
    case TOKEN_LET:
        return "'let'";
    case TOKEN_FUNC:
        return "'func'";
    case TOKEN_STRUCT:
        return "'struct'";
    case TOKEN_RETURN:
        return "'return'";
    case TOKEN_IF:
        return "'if'";
    case TOKEN_ELSE:
        return "'else'";
    case TOKEN_TRUE:
        return "'true'";
    case TOKEN_FALSE:
        return "'false'";
    }

    return "unknown token";
}

static Token lexer_number(Lexer *lexer) {
    const char *begin = lexer->source + lexer->pos;

    while (isdigit(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    TokenType type = TOKEN_INT;
    if (lexer_peek(lexer) == '.') {
        lexer_eat(lexer);

        while (isdigit(lexer_peek(lexer))) {
            lexer_eat(lexer);
        }

        type = TOKEN_FLOAT;
    }

    const char *end = lexer->source + lexer->pos;
    size_t length = end - begin;

    StringRef ref = {.data = begin, .length = length};

    return token_create_ref(lexer, type, ref);
}

static bool is_ident_char(char ch) { return isalnum(ch) || ch == '_'; }

static Token lexer_identifier(Lexer *lexer) {
    const char *begin = lexer->source + lexer->pos;

    while (is_ident_char(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    const char *end = lexer->source + lexer->pos;
    size_t length = end - begin;

    StringRef ref = {.data = begin, .length = length};

    if (string_ref_equals_cstr(ref, "let")) {
        return token_create_ref(lexer, TOKEN_LET, ref);
    }

    if (string_ref_equals_cstr(ref, "if")) {
        return token_create_ref(lexer, TOKEN_IF, ref);
    }

    if (string_ref_equals_cstr(ref, "else")) {
        return token_create_ref(lexer, TOKEN_ELSE, ref);
    }

    if (string_ref_equals_cstr(ref, "func")) {
        return token_create_ref(lexer, TOKEN_FUNC, ref);
    }

    if (string_ref_equals_cstr(ref, "struct")) {
        return token_create_ref(lexer, TOKEN_STRUCT, ref);
    }

    if (string_ref_equals_cstr(ref, "return")) {
        return token_create_ref(lexer, TOKEN_RETURN, ref);
    }

    if (string_ref_equals_cstr(ref, "true")) {
        return token_create_ref(lexer, TOKEN_TRUE, ref);
    }

    if (string_ref_equals_cstr(ref, "false")) {
        return token_create_ref(lexer, TOKEN_FALSE, ref);
    }

    return token_create_ref(lexer, TOKEN_IDENT, ref);
}

Lexer lexer_create(const char *source, Diagnostics *diagnostics) {
    return (Lexer){
        .source = source,
        .pos = 0,
        .line = 1,
        .column = 1,
        .start_line = 1,
        .start_column = 1,
        .diagnostics = diagnostics,
    };
}

// An invalid token used to reach the parser silently; report it here instead.
static Token lexer_invalid(Lexer *lexer, char ch) {
    Token token = token_create(lexer, TOKEN_INVALID);

    diag_error(lexer->diagnostics, GAB_ERR_SYNTAX, token_span(token), "unexpected character '%c'", ch);

    return token;
}

Token lexer_handle_eq(Lexer *lexer, TokenType base_tok, TokenType eq_tok) {
    if (lexer_peek(lexer) == '=') {
        lexer_eat(lexer);
        return token_create(lexer, eq_tok);
    }
    return token_create(lexer, base_tok);
}

Token lexer_handle_op_eq(Lexer *lexer, TokenType base_tok, TokenType eq_tok, char op_ch, TokenType op_tok) {
    char ch = lexer_peek(lexer);
    if (ch == '=') {
        lexer_eat(lexer);
        return token_create(lexer, eq_tok);
    }

    if (ch == op_ch) {
        lexer_eat(lexer);
        return token_create(lexer, op_tok);
    }

    if (base_tok == TOKEN_INVALID) {
        return lexer_invalid(lexer, op_ch);
    }

    return token_create(lexer, base_tok);
}

Token lexer_next(Lexer *lexer) {
    while (isspace(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    lexer->start_line = lexer->line;
    lexer->start_column = lexer->column;

    if (isdigit(lexer_peek(lexer)) || lexer_peek(lexer) == '.') {
        return lexer_number(lexer);
    }

    if (isalpha(lexer_peek(lexer)) || lexer_peek(lexer) == '_') { // Start with a letter or '_'
        return lexer_identifier(lexer);
    }

    // Not eaten: advancing past the terminator would read out of bounds on any
    // further call.
    if (lexer_peek(lexer) == '\0') {
        return token_create(lexer, TOKEN_EOF);
    }

    char ch = lexer_peek(lexer);
    lexer_eat(lexer);

    switch (ch) {
    case '+':
        return token_create(lexer, TOKEN_PLUS);
    case '-':
        return token_create(lexer, TOKEN_MINUS);
    case '*':
        return token_create(lexer, TOKEN_MUL);
    case '/':
        return token_create(lexer, TOKEN_DIV);
    case '(':
        return token_create(lexer, TOKEN_LPAREN);
    case ')':
        return token_create(lexer, TOKEN_RPAREN);
    case '{':
        return token_create(lexer, TOKEN_LBRACE);
    case '}':
        return token_create(lexer, TOKEN_RBRACE);
    case '=':
        return lexer_handle_eq(lexer, TOKEN_ASSIGN, TOKEN_EQUAL);
    case '!':
        return lexer_handle_eq(lexer, TOKEN_NOT, TOKEN_NEQUAL);
    case '<':
        return lexer_handle_eq(lexer, TOKEN_LESS, TOKEN_LEQUAL);
    case '>':
        return lexer_handle_eq(lexer, TOKEN_GREATER, TOKEN_GEQUAL);
    case '&':
        return lexer_handle_op_eq(lexer, TOKEN_INVALID, TOKEN_INVALID, '&', TOKEN_AND);
    case '|':
        return lexer_handle_op_eq(lexer, TOKEN_INVALID, TOKEN_INVALID, '|', TOKEN_OR);
    case ';':
        return token_create(lexer, TOKEN_SEMICOLON);
    case ':':
        return token_create(lexer, TOKEN_COLON);
    case ',':
        return token_create(lexer, TOKEN_COMMA);
    default:
        return lexer_invalid(lexer, ch);
    }
}
