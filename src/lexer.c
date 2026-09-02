#include "lexer.h"
#include "string/string.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
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

Token token_create_value(Lexer *lexer, TokenType type, TokenValue value) {
    return (Token){.type = type, .value = value, .line = lexer->start_line, .column = lexer->start_column};
}

Token token_create_ref(Lexer *lexer, TokenType type, StringRef ref) {
    return (Token){.type = type, .lexeme = ref, .line = lexer->start_line, .column = lexer->start_column};
}

Span token_span(Token token) { return (Span){.line = token.line, .column = token.column}; }

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
    case TOKEN_STRING:
        return "a string literal";
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
    case TOKEN_MOD:
        return "'%'";
    case TOKEN_ASSIGN:
        return "'='";
    case TOKEN_PLUS_EQ:
        return "'+='";
    case TOKEN_MINUS_EQ:
        return "'-='";
    case TOKEN_MUL_EQ:
        return "'*='";
    case TOKEN_DIV_EQ:
        return "'/='";
    case TOKEN_MOD_EQ:
        return "'%='";
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
    case TOKEN_AMP:
        return "'&'";
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
    case TOKEN_LBRACKET:
        return "'['";
    case TOKEN_RBRACKET:
        return "']'";
    case TOKEN_SEMICOLON:
        return "';'";
    case TOKEN_COLON:
        return "':'";
    case TOKEN_COLON_COLON:
        return "'::'";
    case TOKEN_COMMA:
        return "','";
    case TOKEN_DOT:
        return "'.'";
    case TOKEN_DOT_DOT:
        return "'..'";
    case TOKEN_LET:
        return "'let'";
    case TOKEN_FUNC:
        return "'func'";
    case TOKEN_EXTERN:
        return "'extern'";
    case TOKEN_STRUCT:
        return "'struct'";
    case TOKEN_IMPL:
        return "'impl'";
    case TOKEN_BOX:
        return "'box'";
    case TOKEN_MODULE:
        return "'module'";
    case TOKEN_IMPORT:
        return "'import'";
    case TOKEN_RETURN:
        return "'return'";
    case TOKEN_IF:
        return "'if'";
    case TOKEN_ELSE:
        return "'else'";
    case TOKEN_FOR:
        return "'for'";
    case TOKEN_BREAK:
        return "'break'";
    case TOKEN_CONTINUE:
        return "'continue'";
    case TOKEN_TRUE:
        return "'true'";
    case TOKEN_FALSE:
        return "'false'";
    }

    return "unknown token";
}

static Token lexer_number(Lexer *lexer) {
    Span opened = {.line = lexer->start_line, .column = lexer->start_column};
    const char *begin = lexer->source + lexer->pos;

    while (isdigit(lexer_peek(lexer))) {
        lexer_eat(lexer);
    }

    TokenType type = TOKEN_INT;

    if (lexer_peek(lexer) == '.' && lexer->source[lexer->pos + 1] != '.') {
        lexer_eat(lexer);

        while (isdigit(lexer_peek(lexer))) {
            lexer_eat(lexer);
        }

        type = TOKEN_FLOAT;
    }

    if (type == TOKEN_FLOAT) {
        return token_create_value(lexer, type, (TokenValue){.as_float = strtof(begin, NULL)});
    }

    errno = 0;
    long value = strtol(begin, NULL, 10);

    if (errno == ERANGE || value > INT32_MAX) {
        diag_error(lexer->diagnostics, GAB_ERR_TYPE, opened, "integer literal is out of range");

        return token_create(lexer, TOKEN_INVALID);
    }

    return token_create_value(lexer, type, (TokenValue){.as_int = (int32_t)value});
}

static bool lexer_reserve(Lexer *lexer, size_t needed) {
    if (needed <= lexer->scratch_capacity) {
        return true;
    }

    size_t capacity = lexer->scratch_capacity ? lexer->scratch_capacity : 32;

    while (capacity < needed) {
        capacity *= 2;
    }

    char *grown = arena_alloc(lexer->arena, capacity);

    if (!grown) {
        return false;
    }

    if (lexer->scratch) {
        memcpy(grown, lexer->scratch, lexer->scratch_capacity);
    }

    lexer->scratch = grown;
    lexer->scratch_capacity = capacity;

    return true;
}

static int escape_value(char ch) {
    switch (ch) {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case '0':
        return '\0';
    case '\\':
        return '\\';
    case '"':
        return '"';
    default:
        return -1;
    }
}

static Token lexer_string(Lexer *lexer) {
    Span opened = {.line = lexer->start_line, .column = lexer->start_column};

    lexer_eat(lexer);

    size_t length = 0;

    while (lexer_peek(lexer) != '"') {
        char ch = lexer_peek(lexer);

        if (ch == '\0' || ch == '\n') {
            diag_error(lexer->diagnostics, GAB_ERR_SYNTAX, opened, "unterminated string literal");

            return token_create(lexer, TOKEN_INVALID);
        }

        if (ch == '\\') {
            Span at = {.line = lexer->line, .column = lexer->column};

            lexer_eat(lexer);

            int value = escape_value(lexer_peek(lexer));

            if (value < 0) {
                diag_error(lexer->diagnostics, GAB_ERR_SYNTAX, at, "unknown escape sequence '\\%c'",
                           lexer_peek(lexer));

                return token_create(lexer, TOKEN_INVALID);
            }

            ch = (char)value;
        }

        if (!lexer_reserve(lexer, length + 1)) {
            diag_error(lexer->diagnostics, GAB_ERR_SYNTAX, opened, "string literal is too long");

            return token_create(lexer, TOKEN_INVALID);
        }

        lexer->scratch[length++] = ch;
        lexer_eat(lexer);
    }

    lexer_eat(lexer);

    String *text = string_from_ref(lexer->strings, (StringRef){.data = lexer->scratch, .length = length});

    return token_create_value(lexer, TOKEN_STRING, (TokenValue){.as_string = text});
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

    if (string_ref_equals_cstr(ref, "for")) {
        return token_create_ref(lexer, TOKEN_FOR, ref);
    }

    if (string_ref_equals_cstr(ref, "break")) {
        return token_create_ref(lexer, TOKEN_BREAK, ref);
    }

    if (string_ref_equals_cstr(ref, "continue")) {
        return token_create_ref(lexer, TOKEN_CONTINUE, ref);
    }

    if (string_ref_equals_cstr(ref, "func")) {
        return token_create_ref(lexer, TOKEN_FUNC, ref);
    }

    if (string_ref_equals_cstr(ref, "struct")) {
        return token_create_ref(lexer, TOKEN_STRUCT, ref);
    }

    if (string_ref_equals_cstr(ref, "impl")) {
        return token_create_ref(lexer, TOKEN_IMPL, ref);
    }

    if (string_ref_equals_cstr(ref, "extern")) {
        return token_create_ref(lexer, TOKEN_EXTERN, ref);
    }

    if (string_ref_equals_cstr(ref, "box")) {
        return token_create_ref(lexer, TOKEN_BOX, ref);
    }

    if (string_ref_equals_cstr(ref, "module")) {
        return token_create_ref(lexer, TOKEN_MODULE, ref);
    }

    if (string_ref_equals_cstr(ref, "import")) {
        return token_create_ref(lexer, TOKEN_IMPORT, ref);
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

Lexer lexer_create(const char *source, Arena *arena, StringPool *strings, Diagnostics *diagnostics) {
    return (Lexer){
        .source = source,
        .pos = 0,
        .line = 1,
        .column = 1,
        .start_line = 1,
        .start_column = 1,
        .diagnostics = diagnostics,
        .arena = arena,
        .strings = strings,
    };
}

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

        if (eq_tok == TOKEN_INVALID) {
            return lexer_invalid(lexer, op_ch);
        }

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

static void lexer_skip_trivia(Lexer *lexer) {
    for (;;) {
        while (isspace(lexer_peek(lexer))) {
            lexer_eat(lexer);
        }

        if (lexer_peek(lexer) != '/') {
            return;
        }

        char next = lexer->source[lexer->pos + 1];

        if (next == '/') {
            while (lexer_peek(lexer) != '\n' && lexer_peek(lexer) != '\0') {
                lexer_eat(lexer);
            }
            continue;
        }

        if (next == '*') {
            int line = lexer->line;
            int column = lexer->column;

            lexer_eat(lexer);
            lexer_eat(lexer);

            while (!(lexer_peek(lexer) == '*' && lexer->source[lexer->pos + 1] == '/')) {
                if (lexer_peek(lexer) == '\0') {
                    diag_error(lexer->diagnostics, GAB_ERR_SYNTAX, (Span){.line = line, .column = column},
                               "unterminated block comment");
                    return;
                }

                lexer_eat(lexer);
            }

            lexer_eat(lexer);
            lexer_eat(lexer);
            continue;
        }

        return;
    }
}

Token lexer_next(Lexer *lexer) {
    lexer_skip_trivia(lexer);

    lexer->start_line = lexer->line;
    lexer->start_column = lexer->column;

    if (isdigit(lexer_peek(lexer)) ||
        (lexer_peek(lexer) == '.' && isdigit((unsigned char)lexer->source[lexer->pos + 1]))) {
        return lexer_number(lexer);
    }

    if (isalpha(lexer_peek(lexer)) || lexer_peek(lexer) == '_') {
        return lexer_identifier(lexer);
    }

    if (lexer_peek(lexer) == '"') {
        return lexer_string(lexer);
    }

    if (lexer_peek(lexer) == '\0') {
        return token_create(lexer, TOKEN_EOF);
    }

    char ch = lexer_peek(lexer);
    lexer_eat(lexer);

    switch (ch) {
    case '+':
        return lexer_handle_eq(lexer, TOKEN_PLUS, TOKEN_PLUS_EQ);
    case '-':
        return lexer_handle_eq(lexer, TOKEN_MINUS, TOKEN_MINUS_EQ);
    case '*':
        return lexer_handle_eq(lexer, TOKEN_MUL, TOKEN_MUL_EQ);
    case '/':
        return lexer_handle_eq(lexer, TOKEN_DIV, TOKEN_DIV_EQ);
    case '%':
        return lexer_handle_eq(lexer, TOKEN_MOD, TOKEN_MOD_EQ);
    case '(':
        return token_create(lexer, TOKEN_LPAREN);
    case ')':
        return token_create(lexer, TOKEN_RPAREN);
    case '{':
        return token_create(lexer, TOKEN_LBRACE);
    case '}':
        return token_create(lexer, TOKEN_RBRACE);
    case '[':
        return token_create(lexer, TOKEN_LBRACKET);
    case ']':
        return token_create(lexer, TOKEN_RBRACKET);
    case '=':
        return lexer_handle_eq(lexer, TOKEN_ASSIGN, TOKEN_EQUAL);
    case '!':
        return lexer_handle_eq(lexer, TOKEN_NOT, TOKEN_NEQUAL);
    case '<':
        return lexer_handle_eq(lexer, TOKEN_LESS, TOKEN_LEQUAL);
    case '>':
        return lexer_handle_eq(lexer, TOKEN_GREATER, TOKEN_GEQUAL);
    case '&':
        return lexer_handle_op_eq(lexer, TOKEN_AMP, TOKEN_INVALID, '&', TOKEN_AND);
    case '|':
        return lexer_handle_op_eq(lexer, TOKEN_INVALID, TOKEN_INVALID, '|', TOKEN_OR);
    case ';':
        return token_create(lexer, TOKEN_SEMICOLON);
    case ':':

        return lexer_handle_op_eq(lexer, TOKEN_COLON, TOKEN_INVALID, ':', TOKEN_COLON_COLON);
    case ',':
        return token_create(lexer, TOKEN_COMMA);
    case '.':
        if (lexer_peek(lexer) == '.') {
            lexer_eat(lexer);
            return token_create(lexer, TOKEN_DOT_DOT);
        }

        return token_create(lexer, TOKEN_DOT);
    default:
        return lexer_invalid(lexer, ch);
    }
}
