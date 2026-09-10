#ifndef GAB_PARSER_H
#define GAB_PARSER_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "memory/arena.h"
#include "string/string_pool.h"

#include <stdbool.h>
#include <stddef.h>

bool parse_file(const char *source, Arena *arena, StringPool *strings, ASTFile **out,
                Diagnostics *diagnostics);

bool parse_header(const char *source, Arena *arena, StringPool *strings, ASTFile **out,
                  Diagnostics *diagnostics);

bool parse_module(const char *const *sources, size_t count, const char *const *names, Arena *arena,
                  StringPool *strings, ASTModule **out, Diagnostics *diagnostics);

#endif
