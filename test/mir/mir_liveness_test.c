#include "mir/mir_liveness.h"
#include "mir/mir_lower.h"
#include "support/run.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    TestContext ctx;
    Scope *scope;
    ASTUnit *unit;
    ResolvedUnit *resolved;
    MIRFunction *ir;
} Lowered;

static void lower(Lowered *lowered, const char *source) {
    test_context_init(&lowered->ctx);

    lowered->scope = scope_create(lowered->ctx.arena, &lowered->ctx.strings, NULL);
    lowered->unit = ast_unit_create(lowered->ctx.arena);

    assert(test_resolve_ir(&lowered->ctx, lowered->scope, &lowered->unit, NULL, &lowered->resolved, source));

    for (size_t i = 0; i < lowered->unit->statements.size; i++) {
        ASTStmt *stmt = lowered->unit->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL && stmt->func_decl.body) {
            lowered->ir = mir_build_function(lowered->ctx.arena, lowered->scope->type_registry,
                                             &lowered->resolved->facts, stmt->func_decl.function,
                                             &stmt->func_decl.params, stmt->func_decl.body);
            return;
        }
    }

    assert(false && "the unit declares no function with a body");
}

static void lowered_free(Lowered *lowered) { test_context_free(&lowered->ctx); }

static void test_a_parameter_read_at_the_end_is_live_throughout(void) {
    Lowered lowered;
    lower(&lowered, "func f(a: int, b: int): int { let c: int = b; return a; }");

    Liveness *liveness = mir_liveness_compute(lowered.ctx.arena, lowered.ir);

    assert(mir_live_on_entry(liveness, lowered.ir->entry, lowered.ir->params[0]));

    lowered_free(&lowered);
}

static void test_a_value_never_read_again_is_not_live(void) {
    Lowered lowered;
    lower(&lowered, "func f(a: int): int { let b: int = a; return 0; }");

    Liveness *liveness = mir_liveness_compute(lowered.ctx.arena, lowered.ir);

    const MIRBlock *last = lowered.ir->blocks[lowered.ir->block_count - 1];

    assert(!mir_live_on_exit(liveness, last->id, lowered.ir->params[0]));

    lowered_free(&lowered);
}

/* The counter is written in the body and read in the header, so it is live leaving the body. */
static void test_a_value_read_on_the_next_turn_is_live_across_the_back_edge(void) {
    Lowered lowered;
    lower(&lowered, "func f(n: int): int { for let i: int = 0; i < n; i = i + 1 { } return 0; }");

    Liveness *liveness = mir_liveness_compute(lowered.ctx.arena, lowered.ir);

    const MIRBlock *latch = NULL;

    for (size_t i = 0; i < lowered.ir->block_count; i++) {
        MIRBlockId successors[2];
        size_t count = mir_block_successors(lowered.ir->blocks[i], successors);

        for (size_t s = 0; s < count; s++) {
            if (successors[s].id < lowered.ir->blocks[i]->id.id) {
                latch = lowered.ir->blocks[i];
            }
        }
    }

    assert(latch != NULL);

    MIRValueId counter = MIR_NO_VALUE;

    for (size_t i = 0; i < lowered.ir->value_count; i++) {
        const MIRValueInfo *info = mir_value_info(lowered.ir, (MIRValueId){(uint32_t)i});

        if (info->binding && info->binding->kind == BINDING_VAR) {
            counter = (MIRValueId){(uint32_t)i};
        }
    }

    assert(!mir_value_is_none(counter));

    assert(mir_live_on_exit(liveness, latch->id, counter));

    lowered_free(&lowered);
}

static void test_a_value_is_not_live_after_its_last_read(void) {
    Lowered lowered;
    lower(&lowered, "func f(a: int, b: int): int { return a + b; }");

    Liveness *liveness = mir_liveness_compute(lowered.ctx.arena, lowered.ir);

    const MIRBlock *entry = mir_block_at(lowered.ir, lowered.ir->entry);

    size_t add_at = entry->inst_count;

    for (size_t i = 0; i < entry->inst_count; i++) {
        if (entry->insts[i].op == MIR_ADD) {
            add_at = i;
        }
    }

    assert(add_at < entry->inst_count);

    assert(!mir_live_after(liveness, entry->id, add_at, lowered.ir->params[0], MIR_WHOLE_VALUE));
    assert(mir_live_after(liveness, entry->id, add_at, entry->insts[add_at].result, MIR_WHOLE_VALUE));

    lowered_free(&lowered);
}

/* A place names every value its path needs, so an index is read where the place is used. */
static void test_an_index_is_live_where_the_place_using_it_is(void) {
    Lowered lowered;
    lower(&lowered, "func f(xs: array<int, 4>, i: int): int { return xs[i]; }");

    Liveness *liveness = mir_liveness_compute(lowered.ctx.arena, lowered.ir);

    assert(mir_live_on_entry(liveness, lowered.ir->entry, lowered.ir->params[1]));

    lowered_free(&lowered);
}

int main(void) {
    test_a_parameter_read_at_the_end_is_live_throughout();
    test_a_value_never_read_again_is_not_live();
    test_a_value_read_on_the_next_turn_is_live_across_the_back_edge();
    test_a_value_is_not_live_after_its_last_read();
    test_an_index_is_live_where_the_place_using_it_is();

    printf("mir_liveness_test passed\n");

    return 0;
}
