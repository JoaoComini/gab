#ifndef GAB_AST_IDENT_H
#define GAB_AST_IDENT_H

#include "diagnostics.h"
#include "memory/arena.h"
#include "string/string.h"
#include "string/string_ref.h"

/* A name the source wrote. Interned as it is parsed, so every stage past the parser compares
 * pointers, and carrying its own span so a diagnostic about the name points at the name. */
typedef struct ASTIdent {
    String *name;

    Span span;
} ASTIdent;

ASTIdent *ast_ident_create(Arena *arena, StringPool *strings, Span span, StringRef name);

#endif
