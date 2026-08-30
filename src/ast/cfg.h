#ifndef GAB_AST_CFG_H
#define GAB_AST_CFG_H

#include "arena.h"
#include "ast/stmt.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct CFGBlock CFGBlock;

typedef struct {
    ASTStmt **data;
    size_t size;
    size_t capacity;
} CFGStmtList;

struct CFGBlock {
    CFGStmtList stmts;

    CFGBlock *sequential;
    CFGBlock *branch;

    size_t index;
};

typedef struct {
    CFGBlock **blocks;
    size_t block_count;
    size_t block_capacity;

    CFGBlock *entry;

    Arena *arena;
} CFG;

CFG *cfg_build(Arena *arena, ASTStmt *body);

#endif
