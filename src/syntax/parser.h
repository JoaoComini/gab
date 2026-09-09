#ifndef GAB_PARSER_H
#define GAB_PARSER_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "memory/arena.h"
#include "string/string_pool.h"

#include <stdbool.h>
#include <stddef.h>

/* False where the source does not parse, so a file exists only once it holds together. */
bool parse_file(const char *source, Arena *arena, StringPool *strings, ASTFile **out,
                Diagnostics *diagnostics);

/* Every file of one module, which is one namespace however many files write it: false where they do
 * not agree on the module they declare. 'names' names each source in a diagnostic about it. */
bool parse_module(const char *const *sources, size_t count, const char *const *names, Arena *arena,
                  StringPool *strings, ASTModule **out, Diagnostics *diagnostics);

#endif
