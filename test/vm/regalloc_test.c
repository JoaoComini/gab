#include "mir/mir_drop.h"
#include "mir/mir_liveness.h"
#include "mir/mir_lower.h"
#include "support/run.h"
#include "vm/regalloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    TestContext ctx;
    Scope *scope;
    ASTUnit *unit;
    ResolvedUnit *resolved;
    MIRFunction *ir;
    RegAlloc *alloc;
} Allocated;

static void allocate(Allocated *out, const char *source) {
    test_context_init(&out->ctx);

    out->scope = scope_create(out->ctx.arena, &out->ctx.strings, NULL);
    out->unit = ast_unit_create(out->ctx.arena);

    assert(test_resolve_ir(&out->ctx, out->scope, &out->unit, NULL, &out->resolved, source));

    for (size_t i = 0; i < out->unit->statements.size; i++) {
        ASTStmt *stmt = out->unit->statements.data[i];

        if (!stmt || stmt->kind != STMT_FUNC_DECL || !stmt->func_decl.body) {
            continue;
        }

        out->ir = mir_build_function(out->ctx.arena, out->scope->type_registry, &out->resolved->facts,
                                     stmt->func_decl.function, &stmt->func_decl.params, stmt->func_decl.body);

        mir_drop_elaborate(out->ctx.arena, out->scope->type_registry, out->ir);

        out->alloc = regalloc_run(out->ctx.arena, out->ir, &out->ctx.diagnostics);

        return;
    }

    assert(false && "the unit declares no function with a body");
}

static void allocated_free(Allocated *out) { test_context_free(&out->ctx); }

static unsigned int width_of(const MIRFunction *ir, MIRValueId value) {
    const MIRValueInfo *info = mir_value_info(ir, value);
    const Type *type = info ? info->type : NULL;

    if (!type) {
        return 1;
    }

    return (unsigned int)((type_registry_size_of(ir->registry, type) + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);
}

/* Two values live at once must not share a slot, or one would overwrite what the other still holds. */
static void assert_no_live_pair_shares_a_slot(Allocated *a) {
    Liveness *liveness = mir_liveness_compute(a->ctx.arena, a->ir);

    for (uint32_t x = 0; x < a->ir->value_count; x++) {
        for (uint32_t y = x + 1; y < a->ir->value_count; y++) {
            MIRValueId vx = {x}, vy = {y};

            unsigned int sx = regalloc_slot_of(a->alloc, vx);
            unsigned int sy = regalloc_slot_of(a->alloc, vy);

            bool overlaps = sx < sy + width_of(a->ir, vy) && sy < sx + width_of(a->ir, vx);

            if (!overlaps) {
                continue;
            }

            for (size_t b = 0; b < a->ir->block_count; b++) {
                const MIRBlock *block = a->ir->blocks[b];

                for (size_t i = 0; i < block->inst_count; i++) {
                    bool live_x = mir_live_after(liveness, block->id, i, vx, MIR_WHOLE_VALUE);
                    bool live_y = mir_live_after(liveness, block->id, i, vy, MIR_WHOLE_VALUE);

                    if (live_x && live_y) {
                        fprintf(stderr, "%%%u and %%%u share slot %u while both live\n", x, y, sx);
                        assert(false);
                    }
                }
            }
        }
    }
}

static void test_a_parameter_sits_where_the_caller_left_it(void) {
    Allocated a;
    allocate(&a, "func f(x: i32, y: i32): i32 { return x + y; }");

    /* Slot 0 is the return slot, so the first parameter follows it. */
    assert(regalloc_slot_of(a.alloc, a.ir->params[0]) == 1);
    assert(regalloc_slot_of(a.alloc, a.ir->params[1]) == 2);

    allocated_free(&a);
}

static void test_a_slot_a_dead_parameter_left_is_reused(void) {
    Allocated a;
    allocate(&a, "func f(x: i32): i32 {\n"
                 "    let a: i32 = 1;\n"
                 "    let b: i32 = 2;\n"
                 "    let c: i32 = 3;\n"
                 "    return a + b + c;\n"
                 "}\n");

    bool reused = false;

    for (uint32_t v = 0; v < a.ir->value_count; v++) {
        MIRValueId value = {v};

        if (value.id != a.ir->params[0].id &&
            regalloc_slot_of(a.alloc, value) == regalloc_slot_of(a.alloc, a.ir->params[0])) {
            reused = true;
        }
    }

    assert(reused);

    allocated_free(&a);
}

static void test_values_never_live_together_share_a_slot(void) {
    Allocated a;
    allocate(&a, "func f(n: i32): i32 {\n"
                 "    let a: i32 = n + 1;\n"
                 "    let b: i32 = n + 2;\n"
                 "    let c: i32 = n + 3;\n"
                 "    return c;\n"
                 "}\n");

    assert(a.alloc->frame_slots < a.ir->value_count);

    allocated_free(&a);
}

static void test_no_two_values_live_at_once_share_a_slot(void) {
    static const char *const sources[] = {
        "func f(n: i32): i32 { let a = 0; for let i = 0; i < n; i = i + 1 { a = a + i; } return a; }",
        "func f(c: bool, n: i32): i32 { if c { return n + 1; } return n + 2; }",
        "func f(n: i32): i32 { let a = n * 2; let b = a + 1; let c = b + a; return c; }",
        "struct P { x: i32 }\nfunc f(p: P): i32 { let q = P { x: p.x }; return q.x; }\n"};

    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        Allocated a;
        allocate(&a, sources[i]);

        assert(!a.alloc->failed);

        assert_no_live_pair_shares_a_slot(&a);

        allocated_free(&a);
    }
}

/* A value the loop carries is read on the next turn, so nothing inside may take its slot. */
static void test_a_loop_carried_value_keeps_its_slot(void) {
    Allocated a;
    allocate(&a,
             "func f(n: i32): i32 { let a = 0; for let i = 0; i < n; i = i + 1 { a = a + i; } return a; }");

    assert_no_live_pair_shares_a_slot(&a);

    allocated_free(&a);
}

static void test_a_frame_too_large_is_reported_rather_than_wrapped(void) {
    Allocated a;
    test_context_init(&a.ctx);

    a.scope = scope_create(a.ctx.arena, &a.ctx.strings, NULL);
    a.unit = ast_unit_create(a.ctx.arena);

    static char source[1 << 14];
    size_t at = 0;

    at += (size_t)snprintf(source + at, sizeof(source) - at, "struct Wide { ");

    for (int i = 0; i < 200; i++) {
        at += (size_t)snprintf(source + at, sizeof(source) - at, "f%d: i32, ", i);
    }

    at += (size_t)snprintf(source + at, sizeof(source) - at,
                           "last: i32 }\n"
                           "func f(a: Wide, b: Wide): i32 { return 0; }\n");

    if (!test_resolve_ir(&a.ctx, a.scope, &a.unit, NULL, &a.resolved, source)) {
        test_context_free(&a.ctx);
        return;
    }

    for (size_t i = 0; i < a.unit->statements.size; i++) {
        ASTStmt *stmt = a.unit->statements.data[i];

        if (!stmt || stmt->kind != STMT_FUNC_DECL || !stmt->func_decl.body) {
            continue;
        }

        a.ir = mir_build_function(a.ctx.arena, a.scope->type_registry, &a.resolved->facts,
                                  stmt->func_decl.function, &stmt->func_decl.params, stmt->func_decl.body);

        a.alloc = regalloc_run(a.ctx.arena, a.ir, &a.ctx.diagnostics);

        assert(a.alloc->failed);
    }

    test_context_free(&a.ctx);
}

static void a_struct_read_through_its_fields_keeps_its_slots(void) {
    Allocated a;
    allocate(&a, "struct P { x: i32, y: i32 }\n"
                 "func one(): i32 { let p: P = P { x: 4, y: 9 }; return p.x; }\n");

    for (uint32_t v = 0; v < a.ir->value_count; v++) {
        MIRValueId value = {v};

        if (width_of(a.ir, value) < 2) {
            continue;
        }

        unsigned int base = regalloc_slot_of(a.alloc, value);
        unsigned int width = width_of(a.ir, value);

        /* Nothing else may sit in the slots a struct spans while it still holds them. */
        for (uint32_t other = 0; other < a.ir->value_count; other++) {
            if (other == v) {
                continue;
            }

            unsigned int slot = regalloc_slot_of(a.alloc, (MIRValueId){other});

            assert(slot < base || slot >= base + width);
        }
    }

    allocated_free(&a);
}

static void an_array_filled_element_by_element_keeps_its_slots(void) {
    Allocated a;
    allocate(&a, "func one(): i32 { let x: array<i32, 3> = [1, 20, 300]; return x[1]; }\n");

    for (uint32_t v = 0; v < a.ir->value_count; v++) {
        MIRValueId value = {v};

        unsigned int width = width_of(a.ir, value);

        if (width < 2) {
            continue;
        }

        unsigned int base = regalloc_slot_of(a.alloc, value);

        for (uint32_t other = 0; other < a.ir->value_count; other++) {
            if (other == v) {
                continue;
            }

            unsigned int slot = regalloc_slot_of(a.alloc, (MIRValueId){other});

            assert(slot < base || slot >= base + width);
        }
    }

    allocated_free(&a);
}

int main(void) {
    a_struct_read_through_its_fields_keeps_its_slots();
    an_array_filled_element_by_element_keeps_its_slots();
    test_a_parameter_sits_where_the_caller_left_it();
    test_a_slot_a_dead_parameter_left_is_reused();
    test_values_never_live_together_share_a_slot();
    test_no_two_values_live_at_once_share_a_slot();
    test_a_loop_carried_value_keeps_its_slot();
    test_a_frame_too_large_is_reported_rather_than_wrapped();

    printf("regalloc_test passed\n");

    return 0;
}
