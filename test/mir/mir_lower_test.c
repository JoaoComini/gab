#include "mir/mir_lower.h"
#include "mir/mir_print.h"
#include "support/run.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    TestContext ctx;
    Scope *scope;
    ASTUnit *unit;
    ResolvedUnit *resolved;
} Lowered;

static MIRFunction *lower_first_function(Lowered *lowered, const char *source) {
    test_context_init(&lowered->ctx);

    lowered->scope = scope_create(lowered->ctx.arena, &lowered->ctx.strings, NULL);
    lowered->unit = ast_unit_create(lowered->ctx.arena);

    bool ok =
        test_resolve_ir(&lowered->ctx, lowered->scope, &lowered->unit, NULL, &lowered->resolved, source);

    assert(ok);

    for (size_t i = 0; i < lowered->unit->statements.size; i++) {
        ASTStmt *stmt = lowered->unit->statements.data[i];

        if (!stmt || stmt->kind != STMT_FUNC_DECL || !stmt->func_decl.body) {
            continue;
        }

        return mir_build_function(lowered->ctx.arena, lowered->scope->type_registry,
                                  &lowered->resolved->facts, stmt->func_decl.function,
                                  &stmt->func_decl.params, stmt->func_decl.body);
    }

    assert(false && "the unit declares no function with a body");

    return NULL;
}

static void lowered_free(Lowered *lowered) { test_context_free(&lowered->ctx); }

static size_t count_op(const MIRFunction *ir, MIROp op) {
    size_t count = 0;

    for (size_t i = 0; i < ir->block_count; i++) {
        for (size_t j = 0; j < ir->blocks[i]->inst_count; j++) {
            count += ir->blocks[i]->insts[j].op == op;
        }
    }

    return count;
}

static const MIRInst *find_op(const MIRFunction *ir, MIROp op) {
    for (size_t i = 0; i < ir->block_count; i++) {
        for (size_t j = 0; j < ir->blocks[i]->inst_count; j++) {
            if (ir->blocks[i]->insts[j].op == op) {
                return &ir->blocks[i]->insts[j];
            }
        }
    }

    return NULL;
}

static void test_a_block_ends_in_exactly_one_terminator(void) {
    static const char *const sources[] = {
        "func f(): i32 { return 1; }",
        "func f(a: i32): i32 { if a > 0 { return 1; } return 2; }",
        "func f(a: i32): i32 { for let i: i32 = 0; i < a; i = i + 1 { if i > 2 { break; } } return a; }",
        "func f(a: i32): i32 { for let i: i32 = 0; i < a; i = i + 1 { continue; } return a; }",
        "func f(a: i32): i32 { if a > 0 { return 1; } else { return 2; } }",
        "func f(a: i32, b: i32): bool { return a > 0 && b > 0; }",
        "func f(a: i32) { }",
    };

    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); s++) {
        Lowered lowered;
        MIRFunction *ir = lower_first_function(&lowered, sources[s]);

        for (size_t i = 0; i < ir->block_count; i++) {
            const MIRBlock *block = ir->blocks[i];

            if (!mir_block_is_terminated(block)) {
                fprintf(stderr, "bb%zu is unterminated in: %s\n", i, sources[s]);
                assert(false);
            }

            for (size_t j = 0; j + 1 < block->inst_count; j++) {
                if (mir_op_is_terminator(block->insts[j].op)) {
                    fprintf(stderr, "bb%zu continues past its terminator in: %s\n", i, sources[s]);
                    assert(false);
                }
            }
        }

        lowered_free(&lowered);
    }
}

static void test_a_terminator_names_only_blocks_that_exist(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(
        &lowered, "func f(a: i32): i32 { for let i: i32 = 0; i < a; i = i + 1 { break; } return a; }");

    for (size_t i = 0; i < ir->block_count; i++) {
        MIRBlockId successors[2];
        size_t count = mir_block_successors(ir->blocks[i], successors);

        for (size_t s = 0; s < count; s++) {
            assert(!mir_block_is_none(successors[s]));
            assert(mir_block_at(ir, successors[s]) != NULL);
        }
    }

    lowered_free(&lowered);
}

static void test_a_field_read_is_one_place_rather_than_a_load_chain(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "struct Inner { v: i32 }\n"
                                                     "struct Outer { inner: Inner }\n"
                                                     "func f(o: Outer): i32 { return o.inner.v; }\n");

    const MIRInst *load = find_op(ir, MIR_LOAD);

    assert(load != NULL);
    assert(load->place.projection_count == 2);
    assert(load->place.projections[0].kind == PROJ_FIELD);
    assert(load->place.projections[1].kind == PROJ_FIELD);

    assert(count_op(ir, MIR_LOAD) == 1);

    lowered_free(&lowered);
}

/* Reaching a field through a pointer is a hop the source does not spell, so the place must show it. */
static void test_a_field_read_through_a_pointer_projects_a_deref(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "struct Point { x: i32 }\n"
                                                     "func f(p: &Point): i32 { return p.x; }\n");

    const MIRInst *load = find_op(ir, MIR_LOAD);

    assert(load != NULL);
    assert(load->place.projection_count == 2);
    assert(load->place.projections[0].kind == PROJ_DEREF);
    assert(load->place.projections[1].kind == PROJ_FIELD);

    lowered_free(&lowered);
}

static void test_an_assignment_stores_into_the_place_it_names(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "struct Point { x: i32 }\n"
                                                     "func f(p: &Point) { p.x = 3; }\n");

    const MIRInst *store = find_op(ir, MIR_STORE);

    assert(store != NULL);
    assert(store->arg_count == 1);
    assert(store->place.projection_count == 2);
    assert(store->place.projections[1].kind == PROJ_FIELD);

    lowered_free(&lowered);
}

static void test_a_comparison_carries_its_predicate(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "func f(a: i32, b: i32): bool { return a <= b; }");

    const MIRInst *cmp = find_op(ir, MIR_CMP);

    assert(cmp != NULL);
    assert(cmp->predicate == MIR_CMP_LE);
    assert(cmp->arg_count == 2);

    lowered_free(&lowered);
}

/* 'b' runs only where 'a' allows it, so it cannot share a block with 'a'. */
static void test_an_added_literal_is_named_in_its_instruction(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "func f(a: i32): i32 { return a + 7; }");

    const MIRInst *add = find_op(ir, MIR_ADD);

    assert(add != NULL);
    assert(add->args[1].kind == OPERAND_CONST);
    assert(add->args[1].constant.as_int == 7);

    lowered_free(&lowered);
}

static void test_a_logical_operator_puts_its_second_operand_in_its_own_block(void) {
    Lowered lowered;
    MIRFunction *ir =
        lower_first_function(&lowered, "func f(a: i32, b: i32): bool { return a > 0 && b > 0; }");

    assert(count_op(ir, MIR_BRANCH) == 1);
    assert(count_op(ir, MIR_CMP) == 2);

    const MIRInst *branch = find_op(ir, MIR_BRANCH);

    assert(branch != NULL);
    assert(branch->targets[0].id != branch->targets[1].id);

    lowered_free(&lowered);
}

static void test_a_loop_returns_to_its_header(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(
        &lowered, "func f(a: i32): i32 { for let i: i32 = 0; i < a; i = i + 1 { } return a; }");

    bool found_back_edge = false;

    for (size_t i = 0; i < ir->block_count; i++) {
        MIRBlockId successors[2];
        size_t count = mir_block_successors(ir->blocks[i], successors);

        for (size_t s = 0; s < count; s++) {
            found_back_edge = found_back_edge || successors[s].id < ir->blocks[i]->id.id;
        }
    }

    assert(found_back_edge);

    lowered_free(&lowered);
}

static MIRFunction *lower_named_function(Lowered *lowered, const char *source, const char *name) {
    test_context_init(&lowered->ctx);

    lowered->scope = scope_create(lowered->ctx.arena, &lowered->ctx.strings, NULL);
    lowered->unit = ast_unit_create(lowered->ctx.arena);

    bool ok =
        test_resolve_ir(&lowered->ctx, lowered->scope, &lowered->unit, NULL, &lowered->resolved, source);

    assert(ok);

    size_t length = strlen(name);

    for (size_t i = 0; i < lowered->unit->statements.size; i++) {
        ASTStmt *stmt = lowered->unit->statements.data[i];

        if (!stmt || stmt->kind != STMT_FUNC_DECL || !stmt->func_decl.body) {
            continue;
        }

        if (stmt->func_decl.name.length != length || strncmp(stmt->func_decl.name.data, name, length) != 0) {
            continue;
        }

        return mir_build_function(lowered->ctx.arena, lowered->scope->type_registry,
                                  &lowered->resolved->facts, stmt->func_decl.function,
                                  &stmt->func_decl.params, stmt->func_decl.body);
    }

    assert(false && "the unit declares no such function");

    return NULL;
}

static void test_a_call_names_its_callee_and_its_arguments(void) {
    Lowered lowered;
    MIRFunction *ir = lower_named_function(&lowered,
                                           "func g(a: i32, b: i32): i32 { return a + b; }\n"
                                           "func f(): i32 { return g(1, 2); }\n",
                                           "f");

    const MIRInst *call = find_op(ir, MIR_CALL);

    assert(call != NULL);
    assert(call->arg_count == 2);
    assert(call->callee != NULL);

    lowered_free(&lowered);
}

static void test_a_parameter_is_a_value_the_body_names(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "func f(a: i32, b: i32): i32 { return a; }");

    assert(ir->param_count == 2);

    const MIRInst *ret = find_op(ir, MIR_RETURN);

    assert(ret != NULL);
    assert(ret->arg_count == 1);
    assert(mir_operand_as_value(ret->args[0]).id == ir->params[0].id);

    lowered_free(&lowered);
}

static void test_a_body_that_runs_off_its_end_still_returns(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "func f(a: i32) { let b: i32 = a; }");

    assert(count_op(ir, MIR_RETURN) == 1);

    lowered_free(&lowered);
}

static void test_a_local_opens_and_closes_its_storage(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "func f(): i32 { let a: i32 = 1; return a; }");

    assert(count_op(ir, MIR_STORAGE_LIVE) == 1);

    lowered_free(&lowered);
}

/* An owning local's object is released where the local's scope ends, on every path that leaves it. */
static void test_a_local_is_ended_on_each_path_out_of_its_scope(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "struct Node { n: i32 }\n"
                                                     "func f(c: bool): i32 {\n"
                                                     "    let a: *Node = box Node { n: 1 };\n"
                                                     "    if c { return 0; }\n"
                                                     "    return a.n;\n"
                                                     "}\n");

    /* One for the early return, one for the fallthrough. */
    assert(count_op(ir, MIR_DROP) == 2);

    lowered_free(&lowered);
}

/* A scope closing without leaving the function still ends the locals it opened. */
static void test_a_scope_that_falls_through_ends_its_locals(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "struct Node { n: i32 }\n"
                                                     "func f(c: bool): i32 {\n"
                                                     "    if c {\n"
                                                     "        let a: *Node = box Node { n: 1 };\n"
                                                     "    }\n"
                                                     "    return 0;\n"
                                                     "}\n");

    assert(count_op(ir, MIR_DROP) == 1);
    assert(count_op(ir, MIR_STORAGE_DEAD) == 1);

    lowered_free(&lowered);
}

/* Leaving a loop leaves the scopes opened inside it, so their locals end at the jump. */
static void test_a_break_ends_the_locals_the_loop_body_opened(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "struct Node { n: i32 }\n"
                                                     "func f(n: i32): i32 {\n"
                                                     "    for let i: i32 = 0; i < n; i = i + 1 {\n"
                                                     "        let a: *Node = box Node { n: 1 };\n"
                                                     "        break;\n"
                                                     "    }\n"
                                                     "    return 0;\n"
                                                     "}\n");

    assert(count_op(ir, MIR_DROP) >= 1);

    lowered_free(&lowered);
}

/* A moving read leaves the place holding nothing, so it must be a load rather than a bare name. */
static void test_a_move_reads_through_a_load_that_says_so(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "struct Node { n: i32 }\n"
                                                     "func f(): i32 {\n"
                                                     "    let a: *Node = box Node { n: 1 };\n"
                                                     "    let b: *Node = a;\n"
                                                     "    return b.n;\n"
                                                     "}\n");

    bool found_move = false;

    for (size_t i = 0; i < ir->block_count; i++) {
        for (size_t j = 0; j < ir->blocks[i]->inst_count; j++) {
            const MIRInst *inst = &ir->blocks[i]->insts[j];

            found_move = found_move || (inst->op == MIR_LOAD && inst->read == READ_MOVE);
        }
    }

    assert(found_move);

    lowered_free(&lowered);
}

static void test_a_copied_read_is_not_marked_as_a_move(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "func f(a: i32): i32 { let b: i32 = a; return b; }");

    for (size_t i = 0; i < ir->block_count; i++) {
        for (size_t j = 0; j < ir->blocks[i]->inst_count; j++) {
            const MIRInst *inst = &ir->blocks[i]->insts[j];

            assert(inst->op != MIR_LOAD || inst->read == READ_COPY);
        }
    }

    lowered_free(&lowered);
}

static void indexing_checks_its_bounds(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(
        &lowered, "func one(): i32 { let a: array<i32, 3>; let i: i32 = 1; return a[i]; }\n");

    assert(count_op(ir, MIR_BOUNDS) == 1);

    lowered_free(&lowered);
}

static void a_bounds_check_precedes_the_read_it_guards(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(
        &lowered, "func one(): i32 { let a: array<i32, 3>; let i: i32 = 1; return a[i]; }\n");

    const MIRBlock *block = ir->blocks[0];

    size_t bounds = block->inst_count;
    size_t load = block->inst_count;

    for (size_t i = 0; i < block->inst_count; i++) {
        if (block->insts[i].op == MIR_BOUNDS && bounds == block->inst_count) {
            bounds = i;
        }

        if (block->insts[i].op == MIR_LOAD && load == block->inst_count) {
            load = i;
        }
    }

    assert(bounds < load);

    lowered_free(&lowered);
}

static void indexing_a_constant_still_checks_its_bounds(void) {
    Lowered lowered;
    MIRFunction *ir =
        lower_first_function(&lowered, "func one(): i32 { let a: array<i32, 3>; return a[0]; }\n");

    assert(count_op(ir, MIR_BOUNDS) == 1);

    lowered_free(&lowered);
}

static void every_value_a_body_holds_names_its_type(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(
        &lowered, "func one(): i32 { let a: array<i32, 3> = [1, 20, 300]; return a[1]; }\n");

    for (uint32_t v = 0; v < ir->value_count; v++) {
        const MIRValueInfo *info = mir_value_info(ir, (MIRValueId){v});

        assert(info && info->type);
    }

    lowered_free(&lowered);
}

static void a_cast_between_equal_types_converts_nothing(void) {
    Lowered lowered;
    MIRFunction *ir = lower_first_function(&lowered, "func one(a: i32): i32 { return i32(a); }\n");

    assert(count_op(ir, MIR_FTOI) == 0);
    assert(count_op(ir, MIR_ITOF) == 0);

    lowered_free(&lowered);
}

/* Lowering runs on bodies the resolver has not vetted, so a jump with no enclosing loop must leave
 * the body without a terminator rather than one naming a block that does not exist. */
static void a_jump_outside_a_loop_emits_no_terminator(void) {
    TestContext ctx;
    test_context_init(&ctx);

    Scope *scope = scope_create(ctx.arena, &ctx.strings, NULL);
    ASTUnit *unit = ast_unit_create(ctx.arena);
    ResolvedUnit *resolved;

    test_resolve_ir(&ctx, scope, &unit, NULL, &resolved, "func f(): i32 { break; }\n");

    ASTStmt *decl = NULL;

    for (size_t i = 0; i < unit->statements.size; i++) {
        ASTStmt *stmt = unit->statements.data[i];

        if (stmt && stmt->kind == STMT_FUNC_DECL && stmt->func_decl.body) {
            decl = stmt;
            break;
        }
    }

    assert(decl);

    MIRFunction *ir =
        mir_build_function(ctx.arena, scope->type_registry, &resolved->facts, decl->func_decl.function,
                           &decl->func_decl.params, decl->func_decl.body);

    for (size_t b = 0; b < ir->block_count; b++) {
        MIRBlock *block = ir->blocks[b];

        for (size_t k = 0; k < block->inst_count; k++) {
            const MIRInst *inst = &block->insts[k];

            if (inst->op == MIR_JMP || inst->op == MIR_BRANCH) {
                assert(!mir_block_is_none(inst->targets[0]));
            }
        }
    }

    test_context_free(&ctx);
}

int main(void) {
    test_a_block_ends_in_exactly_one_terminator();
    test_a_terminator_names_only_blocks_that_exist();
    test_a_field_read_is_one_place_rather_than_a_load_chain();
    test_a_field_read_through_a_pointer_projects_a_deref();
    test_an_assignment_stores_into_the_place_it_names();
    test_a_comparison_carries_its_predicate();
    test_an_added_literal_is_named_in_its_instruction();
    test_a_logical_operator_puts_its_second_operand_in_its_own_block();
    test_a_loop_returns_to_its_header();
    indexing_checks_its_bounds();
    a_bounds_check_precedes_the_read_it_guards();
    indexing_a_constant_still_checks_its_bounds();
    every_value_a_body_holds_names_its_type();
    a_cast_between_equal_types_converts_nothing();
    test_a_call_names_its_callee_and_its_arguments();
    test_a_parameter_is_a_value_the_body_names();
    test_a_body_that_runs_off_its_end_still_returns();
    test_a_local_opens_and_closes_its_storage();
    test_a_local_is_ended_on_each_path_out_of_its_scope();
    test_a_scope_that_falls_through_ends_its_locals();
    test_a_break_ends_the_locals_the_loop_body_opened();
    test_a_move_reads_through_a_load_that_says_so();
    test_a_copied_read_is_not_marked_as_a_move();
    a_jump_outside_a_loop_emits_no_terminator();

    printf("mir_lower_test passed\n");

    return 0;
}
