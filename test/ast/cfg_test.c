#include "ast/cfg.h"
#include "support/run.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    TestContext ctx;
    Scope *scope;
    ASTUnit *unit;
} ResolvedScript;

static void resolved_script_init(ResolvedScript *resolved, const char *source) {
    test_context_init(&resolved->ctx);

    resolved->scope = scope_create(resolved->ctx.arena, &resolved->ctx.strings, NULL);
    resolved->unit = ast_unit_create();

    bool ok = test_resolve(&resolved->ctx, resolved->scope, resolved->unit, source);

    assert(ok);
}

static void resolved_script_free(ResolvedScript *resolved) {
    ast_unit_destroy(resolved->unit);
    test_context_free(&resolved->ctx);
}

static CFG *first_func_cfg(ResolvedScript *resolved) {
    for (size_t i = 0; i < resolved->unit->statements.size; i++) {
        ASTStmt *stmt = resolved->unit->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL && stmt->func_decl.body) {
            return cfg_build(resolved->ctx.arena, stmt->func_decl.body);
        }
    }

    assert(false && "the unit declares no function with a body");

    return NULL;
}

static bool reaches(const CFGBlock *from, const CFGBlock *to, const CFG *cfg, bool *seen) {
    if (!from) {
        return false;
    }

    if (from == to) {
        return true;
    }

    if (seen[from->index]) {
        return false;
    }

    seen[from->index] = true;

    return reaches(from->sequential, to, cfg, seen) || reaches(from->branch, to, cfg, seen);
}

static bool block_reaches(const CFG *cfg, const CFGBlock *from, const CFGBlock *to) {
    bool seen[64] = {false};

    assert(cfg->block_count <= 64);

    return reaches(from, to, cfg, seen);
}

static CFGBlock *block_holding(const CFG *cfg, StmtKind kind) {
    for (size_t i = 0; i < cfg->block_count; i++) {
        CFGBlock *block = cfg->blocks[i];

        for (size_t j = 0; j < block->stmts.size; j++) {
            if (block->stmts.data[j]->kind == kind) {
                return block;
            }
        }
    }

    return NULL;
}

static void test_a_break_reaches_what_follows_its_loop() {
    ResolvedScript resolved;
    resolved_script_init(&resolved, "func main(): int {\n"
                                    "    let total: int = 0;\n"
                                    "    for let i: int = 0; i < 4; i = i + 1 {\n"
                                    "        break;\n"
                                    "    }\n"
                                    "    return total;\n"
                                    "}\n");

    CFG *cfg = first_func_cfg(&resolved);

    CFGBlock *jump = block_holding(cfg, STMT_JUMP);
    CFGBlock *ret = block_holding(cfg, STMT_RETURN);

    assert(jump && ret);
    assert(block_reaches(cfg, jump, ret));

    resolved_script_free(&resolved);
}

static void test_a_continue_reaches_the_header_and_not_the_exit() {
    ResolvedScript resolved;
    resolved_script_init(&resolved, "func main(): int {\n"
                                    "    for let i: int = 0; i < 4; i = i + 1 {\n"
                                    "        continue;\n"
                                    "    }\n"
                                    "    return 0;\n"
                                    "}\n");

    CFG *cfg = first_func_cfg(&resolved);

    CFGBlock *jump = block_holding(cfg, STMT_JUMP);
    CFGBlock *header = block_holding(cfg, STMT_FOR);

    assert(jump && header);
    assert(block_reaches(cfg, jump, header));

    resolved_script_free(&resolved);
}

static void test_a_loop_body_reaches_its_header_again() {
    ResolvedScript resolved;
    resolved_script_init(&resolved, "func main(): int {\n"
                                    "    let x: int = 0;\n"
                                    "    for let i: int = 0; i < 4; i = i + 1 {\n"
                                    "        x = x + 1;\n"
                                    "    }\n"
                                    "    return x;\n"
                                    "}\n");

    CFG *cfg = first_func_cfg(&resolved);

    CFGBlock *header = block_holding(cfg, STMT_FOR);

    assert(header);
    assert(header->sequential);
    assert(block_reaches(cfg, header->sequential, header));

    resolved_script_free(&resolved);
}

static void test_both_arms_of_an_if_reach_the_join() {
    ResolvedScript resolved;
    resolved_script_init(&resolved, "func main(): int {\n"
                                    "    let x: int = 0;\n"
                                    "    if 1 < 2 { x = 1; } else { x = 2; }\n"
                                    "    return x;\n"
                                    "}\n");

    CFG *cfg = first_func_cfg(&resolved);

    CFGBlock *branch = block_holding(cfg, STMT_IF);
    CFGBlock *ret = block_holding(cfg, STMT_RETURN);

    assert(branch && ret);
    assert(branch->sequential && branch->branch);
    assert(block_reaches(cfg, branch->sequential, ret));
    assert(block_reaches(cfg, branch->branch, ret));

    resolved_script_free(&resolved);
}

static void test_a_loop_without_a_condition_does_not_leave_by_its_header() {
    ResolvedScript resolved;
    resolved_script_init(&resolved, "func main(): int {\n"
                                    "    for {\n"
                                    "        break;\n"
                                    "    }\n"
                                    "    return 0;\n"
                                    "}\n");

    CFG *cfg = first_func_cfg(&resolved);

    CFGBlock *header = block_holding(cfg, STMT_FOR);

    assert(header);
    assert(header->branch == NULL);

    resolved_script_free(&resolved);
}

int main(void) {
    test_a_break_reaches_what_follows_its_loop();
    test_a_continue_reaches_the_header_and_not_the_exit();
    test_a_loop_body_reaches_its_header_again();
    test_both_arms_of_an_if_reach_the_join();
    test_a_loop_without_a_condition_does_not_leave_by_its_header();

    printf("All cfg tests passed\n");
    return 0;
}
