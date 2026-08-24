#include "ast/cfg.h"

#include <string.h>

#define CFG_INITIAL_CAPACITY 8

// The arena has no realloc, so growth is allocate-and-copy. Blocks and their
// statement lists are both small and short-lived -- the graph dies with the
// compile -- so the copied-over space is not worth reclaiming.
static void *cfg_grow(Arena *arena, void *data, size_t used, size_t *capacity, size_t element) {
    size_t next = *capacity == 0 ? CFG_INITIAL_CAPACITY : *capacity * 2;
    void *grown = arena_alloc(arena, next * element);

    if (data) {
        memcpy(grown, data, used * element);
    }

    *capacity = next;

    return grown;
}

static CFGBlock *cfg_block_create(CFG *cfg) {
    if (cfg->block_count == cfg->block_capacity) {
        cfg->blocks =
            cfg_grow(cfg->arena, cfg->blocks, cfg->block_count, &cfg->block_capacity, sizeof(CFGBlock *));
    }

    CFGBlock *block = arena_alloc(cfg->arena, sizeof(CFGBlock));

    *block = (CFGBlock){.index = cfg->block_count};

    cfg->blocks[cfg->block_count++] = block;

    return block;
}

static void cfg_block_add(CFG *cfg, CFGBlock *block, ASTStmt *stmt) {
    CFGStmtList *list = &block->stmts;

    if (list->size == list->capacity) {
        list->data = cfg_grow(cfg->arena, list->data, list->size, &list->capacity, sizeof(ASTStmt *));
    }

    list->data[list->size++] = stmt;
}

// Where a 'break' and a 'continue' in the statement being built should go.
// Loops nest, so this is saved and restored around a loop's body rather than
// living on the graph.
typedef struct {
    CFGBlock *brk;
    CFGBlock *cont;
} LoopTargets;

// Appends one statement to the graph and returns the block control reaches
// after it. A straight-line statement lands in 'current' and leaves it
// unchanged; anything that branches ends 'current' and opens whatever blocks
// its arms need.
//
// A NULL return means control does not fall through -- after a 'return', a
// 'break', or a 'continue'. The caller stops adding to that path rather than
// emitting statements nothing can reach.
static CFGBlock *cfg_add_stmt(CFG *cfg, CFGBlock *current, ASTStmt *stmt, LoopTargets loop);

static CFGBlock *cfg_add_list(CFG *cfg, CFGBlock *current, ASTStmtList *list, LoopTargets loop) {
    for (size_t i = 0; i < list->size; i++) {
        if (!current) {
            return NULL;
        }

        current = cfg_add_stmt(cfg, current, list->data[i], loop);
    }

    return current;
}

static CFGBlock *cfg_add_stmt(CFG *cfg, CFGBlock *current, ASTStmt *stmt, LoopTargets loop) {
    if (!stmt) {
        return current;
    }

    switch (stmt->kind) {
    case STMT_BLOCK:
        return cfg_add_list(cfg, current, &stmt->block.list, loop);

    case STMT_IF: {
        // The node itself carries the condition, which runs before either arm
        // and so belongs to the block being left.
        cfg_block_add(cfg, current, stmt);

        CFGBlock *then_block = cfg_block_create(cfg);
        CFGBlock *else_block = cfg_block_create(cfg);

        current->sequential = then_block;
        current->branch = else_block;

        CFGBlock *after_then = cfg_add_stmt(cfg, then_block, stmt->ifstmt.then_block, loop);
        CFGBlock *after_else = cfg_add_stmt(cfg, else_block, stmt->ifstmt.else_block, loop);

        // An arm that cannot fall through reaches no join. Where neither does,
        // there is nothing after the 'if' at all.
        if (!after_then && !after_else) {
            return NULL;
        }

        CFGBlock *join = cfg_block_create(cfg);

        if (after_then) {
            after_then->sequential = join;
        }

        if (after_else) {
            after_else->sequential = join;
        }

        return join;
    }

    case STMT_FOR: {
        current = cfg_add_stmt(cfg, current, stmt->forstmt.init, loop);

        if (!current) {
            return NULL;
        }

        // The header is its own block because the back-edge re-enters it: it
        // is where the condition is tested on every iteration, including the
        // first.
        CFGBlock *header = cfg_block_create(cfg);
        current->sequential = header;

        CFGBlock *body = cfg_block_create(cfg);
        CFGBlock *exit = cfg_block_create(cfg);

        // The post clause is a block of its own so that 'continue' has
        // somewhere to land: it runs the post clause and then re-tests, which
        // is what makes the counting form advance.
        CFGBlock *post = cfg_block_create(cfg);

        cfg_block_add(cfg, header, stmt);
        header->sequential = body;

        // An absent condition never leaves by the header, so the loop is left
        // only by a 'break'. Leaving 'branch' NULL would end the function
        // there, which is not what an infinite loop does.
        if (stmt->forstmt.condition) {
            header->branch = exit;
        }

        CFGBlock *after_body = cfg_add_stmt(cfg, body, stmt->forstmt.body, (LoopTargets){exit, post});

        if (after_body) {
            after_body->sequential = post;
        }

        CFGBlock *after_post = cfg_add_stmt(cfg, post, stmt->forstmt.post, loop);

        if (after_post) {
            after_post->sequential = header;
        }

        return exit;
    }

    case STMT_JUMP:
        cfg_block_add(cfg, current, stmt);

        // A jump outside any loop is an error the resolver reports; the graph
        // just has nowhere to send it.
        current->sequential = stmt->jump.is_break ? loop.brk : loop.cont;

        return NULL;

    case STMT_RETURN:
        cfg_block_add(cfg, current, stmt);

        return NULL;

    default:
        cfg_block_add(cfg, current, stmt);

        return current;
    }
}

CFG *cfg_build(Arena *arena, ASTStmt *body) {
    CFG *cfg = arena_alloc(arena, sizeof(CFG));

    *cfg = (CFG){.arena = arena};

    cfg->entry = cfg_block_create(cfg);

    cfg_add_stmt(cfg, cfg->entry, body, (LoopTargets){NULL, NULL});

    return cfg;
}
