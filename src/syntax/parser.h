#ifndef GAB_PARSER_H
#define GAB_PARSER_H

#include "ast/ast.h"
#include "diagnostics.h"
#include "memory/arena.h"
#include "string/string_pool.h"

#include <stdbool.h>

/* False where the source does not parse, so a unit exists only once it holds together. */
bool parse_unit(const char *source, Arena *arena, StringPool *strings, ASTUnit **out,
                Diagnostics *diagnostics);

#endif
