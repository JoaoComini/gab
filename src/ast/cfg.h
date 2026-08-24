#ifndef GAB_AST_CFG_H
#define GAB_AST_CFG_H

#include "arena.h"
#include "ast/stmt.h"

#include <stdbool.h>
#include <stddef.h>

// The control flow of one function body, as basic blocks over the statements
// the resolver already built. Nothing is lowered: a block holds pointers to
// ASTStmt nodes in the order they execute, so a statement's span and its
// resolved symbols are read straight off the node the parser produced.
//
// This exists so that flow analysis is a pass over a graph rather than a
// second walk of the tree. A walk has to hand-roll every join -- fork the
// state at an 'if', merge the arms after it, thread a 'break' out to whatever
// follows the loop -- and gets each one right separately. A graph states the
// joins once, in the edges, and the dataflow merges at every block with more
// than one predecessor without knowing which construct put it there.
//
// It also decouples how many times the body is examined from how many times it
// is resolved. Resolution declares symbols and is not idempotent, so it may run
// exactly once; the dataflow may then iterate to a fixpoint over the graph as
// many times as convergence takes.

typedef struct CFGBlock CFGBlock;

typedef struct {
    ASTStmt **data;
    size_t size;
    size_t capacity;
} CFGStmtList;

struct CFGBlock {
    // The statements this block runs, in order, with no branch among them: a
    // block is entered at the top and left at the bottom.
    CFGStmtList stmts;

    // Where control goes next. 'sequential' is the fall-through and the only
    // successor of a block ending in nothing branching; 'branch' is the other
    // arm of a conditional. NULL means control leaves the function.
    //
    // The condition itself is the last statement of this block, so which
    // successor is taken is a question about the program, which is exactly the
    // question the dataflow declines to answer -- it merges both.
    CFGBlock *sequential;
    CFGBlock *branch;

    // Position in the graph's block list, so a dataflow pass can index its
    // per-block state by it rather than hashing the pointer.
    size_t index;
};

typedef struct {
    CFGBlock **blocks;
    size_t block_count;
    size_t block_capacity;

    // Where the body starts. The exit is not a block: control leaving the
    // function is a NULL successor, since nothing is analyzed there.
    CFGBlock *entry;

    Arena *arena;
} CFG;

// Builds the graph for one function body. The body must already be resolved --
// every symbol bound, every type settled -- because the blocks hold the very
// nodes the resolver annotated.
CFG *cfg_build(Arena *arena, ASTStmt *body);

#endif
