#ifndef GAB_AST_IDENT_H
#define GAB_AST_IDENT_H

#include "diagnostics.h"
#include "memory/arena.h"
#include "string/string.h"
#include "string/string_ref.h"

typedef struct ASTIdent {
    String *name;

    Span span;
} ASTIdent;

ASTIdent *ast_ident_create(Arena *arena, StringPool *strings, Span span, StringRef name);

#endif
